#ifndef ROLE_MANAGER_H
#define ROLE_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <set>

/**
 * @brief 系统内置角色 ID 常量
 *
 * 内置角色不可删除，可修改权限集合。
 */
namespace BuiltinRoles {
    static constexpr const char* ADMIN    = "admin";     ///< 超级管理员，拥有所有权限
    static constexpr const char* OPERATOR = "operator";  ///< 操作员，可操作设备但不能管理用户
    static constexpr const char* VIEWER   = "viewer";    ///< 查看者，只读权限
}

/**
 * @brief 系统内置权限节点常量
 *
 * 格式：资源.动作，例如 "user.create"
 */
namespace BuiltinPermissions {
    // 用户管理
    static constexpr const char* USER_VIEW    = "user.view";
    static constexpr const char* USER_CREATE  = "user.create";
    static constexpr const char* USER_EDIT    = "user.edit";
    static constexpr const char* USER_DELETE  = "user.delete";

    // 角色管理
    static constexpr const char* ROLE_VIEW    = "role.view";
    static constexpr const char* ROLE_CREATE  = "role.create";
    static constexpr const char* ROLE_EDIT    = "role.edit";
    static constexpr const char* ROLE_DELETE  = "role.delete";

    // 系统
    static constexpr const char* SYSTEM_VIEW    = "system.view";
    static constexpr const char* SYSTEM_RESTART = "system.restart";

    // 配置
    static constexpr const char* CONFIG_VIEW  = "config.view";
    static constexpr const char* CONFIG_EDIT  = "config.edit";

    // 网络
    static constexpr const char* NETWORK_VIEW = "network.view";
    static constexpr const char* NETWORK_EDIT = "network.edit";

    // GPIO / 设备操作
    static constexpr const char* DEVICE_VIEW    = "device.view";
    static constexpr const char* DEVICE_CONTROL = "device.control";

    // OTA
    static constexpr const char* OTA_UPDATE = "ota.update";

    // 文件系统
    static constexpr const char* FS_VIEW   = "fs.view";
    static constexpr const char* FS_MANAGE = "fs.manage";

    // 审计日志
    static constexpr const char* AUDIT_VIEW  = "audit.view";
    static constexpr const char* AUDIT_CLEAR = "audit.clear";
}

/**
 * @brief 权限定义
 */
struct PermissionDef {
    String id;           ///< 权限唯一标识，如 "user.create"
    String name;         ///< 显示名称，如 "创建用户"
    String description;  ///< 描述
    String group;        ///< 分组，如 "用户管理"

    PermissionDef() = default;
    PermissionDef(const String& id_, const String& name_,
                  const String& desc, const String& grp)
        : id(id_), name(name_), description(desc), group(grp) {}
};

/**
 * @brief 角色定义
 */
struct Role {
    String id;                          ///< 角色唯一标识（英文小写）
    String name;                        ///< 显示名称
    String description;                 ///< 描述
    bool   isBuiltin;                   ///< 是否为内置角色（不可删除）
    std::vector<String> permissions;    ///< 拥有的权限 ID 列表

    Role() : isBuiltin(false) {}
    Role(const String& id_, const String& name_,
         const String& desc, bool builtin)
        : id(id_), name(name_), description(desc), isBuiltin(builtin) {}

    /** 检查是否拥有某权限 */
    bool hasPermission(const String& perm) const {
        for (const String& p : permissions) {
            if (p == perm) return true;
        }
        return false;
    }
};

/**
 * @brief 角色管理器
 *
 * 负责角色与权限定义的 CRUD 和持久化。
 * 权限校验入口由 AuthManager 调用本类的 roleHasPermission()。
 */
class RoleManager {
public:
    RoleManager();
    ~RoleManager();

    /**
     * @brief 初始化，加载持久化数据；若无数据则创建内置角色
     * @return 是否成功
     */
    bool initialize();

    // ─── 角色 CRUD ───────────────────────────────────────────────

    /**
     * @brief 创建角色
     * @param id    角色 ID（唯一）
     * @param name  显示名称
     * @param desc  描述
     * @return 是否成功（ID 重复则失败）
     */
    bool createRole(const String& id, const String& name, const String& desc = "");

    /**
     * @brief 删除角色（内置角色不可删除）
     * @param id 角色 ID
     * @return 是否成功
     */
    bool deleteRole(const String& id);

    /**
     * @brief 更新角色基本信息（不修改权限集合）
     * @param id    角色 ID
     * @param name  新显示名称（空则不修改）
     * @param desc  新描述（空则不修改）
     * @return 是否成功
     */
    bool updateRole(const String& id, const String& name, const String& desc = "");

    /**
     * @brief 获取角色
     * @param id 角色 ID
     * @return 指针（不存在返回 nullptr）
     */
    Role* getRole(const String& id);

    /**
     * @brief 获取所有角色列表
     */
    std::vector<Role> getAllRoles() const;

    /**
     * @brief 角色是否存在
     */
    bool roleExists(const String& id) const;

    // ─── 角色权限管理 ────────────────────────────────────────────

    /**
     * @brief 为角色赋予权限
     * @param roleId     角色 ID
     * @param permission 权限 ID
     * @return 是否成功
     */
    bool grantPermission(const String& roleId, const String& permission);

    /**
     * @brief 撤销角色权限
     * @param roleId     角色 ID
     * @param permission 权限 ID
     * @return 是否成功
     */
    bool revokePermission(const String& roleId, const String& permission);

    /**
     * @brief 批量替换角色的权限集合
     * @param roleId      角色 ID
     * @param permissions 新权限列表
     * @return 是否成功
     */
    bool setRolePermissions(const String& roleId, const std::vector<String>& permissions);

    /**
     * @brief 获取角色的权限列表
     * @param roleId 角色 ID
     * @return 权限 ID 列表（角色不存在返回空）
     */
    std::vector<String> getRolePermissions(const String& roleId) const;

    /**
     * @brief 判断角色是否拥有某权限
     * @param roleId     角色 ID
     * @param permission 权限 ID
     */
    bool roleHasPermission(const String& roleId, const String& permission) const;

    // ─── 权限定义管理 ─────────────────────────────────────────────

    /**
     * @brief 注册权限定义（供 initializeBuiltinPermissions 及外部模块调用）
     */
    void registerPermission(const PermissionDef& perm);

    /**
     * @brief 获取所有已注册权限定义
     */
    std::vector<PermissionDef> getAllPermissions() const;

    /**
     * @brief 获取按 group 分组的权限（返回 group→[PermissionDef] 映射）
     */
    std::map<String, std::vector<PermissionDef>> getPermissionsByGroup() const;

    // ─── 序列化 ──────────────────────────────────────────────────

    /**
     * @brief 将所有角色序列化为 JSON 字符串
     */
    String toJson() const;

    /**
     * @brief 将单个角色序列化为 JSON
     * @param id 角色 ID
     */
    String roleToJson(const String& id) const;

    // ─── 持久化 ──────────────────────────────────────────────────

    bool saveToStorage();
    bool loadFromStorage();

private:
    std::map<String, Role>          roles;       ///< 角色表 key=roleId
    std::map<String, PermissionDef> permDefs;    ///< 权限定义表 key=permId

    void initializeBuiltinRoles();
    void initializeBuiltinPermissions();

    /** 将 Role 写入 JsonObject */
    static void roleToJsonObj(const Role& role, JsonObject& obj);
};

#endif // ROLE_MANAGER_H
