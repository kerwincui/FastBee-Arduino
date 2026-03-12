"""
LittleFS 文件过滤脚本

功能：
- 在构建 LittleFS 镜像前，临时移除不需要的文件
- www/ 目录下（除 assets/ 外）只保留 .gz 文件
- 保留 config/、logs/、var/、www/assets/ 的所有文件
- 构建完成后恢复被移动的文件

用法：
  在 platformio.ini 中添加：
  extra_scripts = pre:scripts/filter_littlefs.py
"""

import os
import shutil
import atexit
from SCons.Script import Import, COMMAND_LINE_TARGETS

# 导入 PlatformIO 环境
Import("env")

# 数据目录路径
DATA_DIR = os.path.join(env.subst("$PROJECT_DIR"), "data")
WWW_DIR = os.path.join(DATA_DIR, "www")
ASSETS_DIR = os.path.join(WWW_DIR, "assets")

# 临时目录用于存放被过滤的文件
TEMP_DIR = os.path.join(env.subst("$PROJECT_DIR"), ".pio", "filtered_files")

# 需要过滤的文件扩展名（这些原始文件将被跳过，只保留 .gz 版本）
FILTER_EXTENSIONS = ['.html', '.js', '.css']


def should_filter_file(filepath):
    """
    判断文件是否应该被过滤（暂时移除）
    
    规则：
    - www/assets/ 目录下的文件不过滤（保留图标、图片等）
    - www/ 其他目录下的原始文件（.html/.js/.css）应该被过滤
    - 其他目录（config/、logs/、var/）的文件不过滤
    """
    # 规范化路径以便比较
    filepath = os.path.normpath(filepath)
    www_dir = os.path.normpath(WWW_DIR)
    assets_dir = os.path.normpath(ASSETS_DIR)
    
    # 如果不在 www/ 目录下，不过滤
    if not filepath.startswith(www_dir):
        return False
    
    # 如果在 assets/ 目录下，不过滤
    if filepath.startswith(assets_dir):
        return False
    
    # 如果是原始文件（有 .gz 版本），则应该过滤
    ext = os.path.splitext(filepath)[1].lower()
    if ext in FILTER_EXTENSIONS:
        gz_path = filepath + '.gz'
        # 只有当对应的 .gz 文件存在时才过滤
        if os.path.exists(gz_path):
            return True
    
    return False


def get_filtered_files():
    """
    获取所有需要过滤的文件列表
    """
    filtered = []
    
    for root, dirs, files in os.walk(WWW_DIR):
        for filename in files:
            filepath = os.path.join(root, filename)
            if should_filter_file(filepath):
                filtered.append(filepath)
    
    return filtered


def move_filtered_files():
    """
    将需要过滤的文件移动到临时目录
    """
    # 确保临时目录存在
    if os.path.exists(TEMP_DIR):
        shutil.rmtree(TEMP_DIR)
    os.makedirs(TEMP_DIR)
    
    filtered_files = get_filtered_files()
    
    if not filtered_files:
        print("  没有需要过滤的文件")
        return []
    
    moved_files = []
    
    for filepath in filtered_files:
        # 计算在临时目录中的相对路径
        rel_path = os.path.relpath(filepath, DATA_DIR)
        temp_path = os.path.join(TEMP_DIR, rel_path)
        
        # 确保目标目录存在
        os.makedirs(os.path.dirname(temp_path), exist_ok=True)
        
        # 移动文件
        shutil.move(filepath, temp_path)
        moved_files.append((filepath, temp_path))
        
        # 计算相对于 www 目录的路径用于显示
        rel_display = os.path.relpath(filepath, WWW_DIR)
        print(f"  过滤: {rel_display}")
    
    print(f"  共过滤 {len(moved_files)} 个原始文件")
    
    return moved_files


def restore_filtered_files():
    """
    从临时目录恢复被移动的文件
    """
    if not os.path.exists(TEMP_DIR):
        return
    
    restored_count = 0
    
    for root, dirs, files in os.walk(TEMP_DIR):
        for filename in files:
            temp_path = os.path.join(root, filename)
            rel_path = os.path.relpath(temp_path, TEMP_DIR)
            original_path = os.path.join(DATA_DIR, rel_path)
            
            # 确保目标目录存在
            os.makedirs(os.path.dirname(original_path), exist_ok=True)
            
            # 恢复文件
            shutil.move(temp_path, original_path)
            restored_count += 1
    
    # 清理临时目录
    shutil.rmtree(TEMP_DIR)
    
    if restored_count > 0:
        print(f"  已恢复 {restored_count} 个原始文件")


def filter_files_now():
    """
    立即执行文件过滤（在脚本加载时）
    """
    print("\n========================================")
    print("  LittleFS 文件过滤（仅上传 .gz 文件）")
    print("========================================")
    print(f"数据目录: {DATA_DIR}")
    print(f"\n[Filter] 过滤 www/ 目录下的原始文件...")
    
    global _moved_files
    _moved_files = move_filtered_files()
    
    print("========================================\n")


def restore_files_on_exit():
    """
    在脚本退出时恢复文件（通过 atexit 注册）
    """
    if not _moved_files:
        return
    
    print("\n========================================")
    print("  恢复被过滤的文件")
    print("========================================")
    
    restore_filtered_files()
    
    print("========================================\n")


# 全局变量保存被移动的文件列表
_moved_files = []

# 只在 uploadfs 或 buildfs 目标时执行文件过滤
if "uploadfs" in COMMAND_LINE_TARGETS or "buildfs" in COMMAND_LINE_TARGETS:
    # 立即执行文件过滤（在 mklittlefs 运行之前）
    filter_files_now()
    
    # 注册退出时恢复文件
    atexit.register(restore_files_on_exit)
