#include "security/RoleManager.h"
#include "systems/LoggerSystem.h"
#include <LittleFS.h>

static const char* ROLES_FILE = "/config/roles.json";

// ─── 构造 / 析构 ─────────────────────────────────────────────────────────────

RoleManager::RoleManager() {
    // 使用文件系统存储
}

RoleManager::~RoleManager() {
    saveToStorage();
}

// ─── 初始化 ───────────────────────────────────────────────────────────────────

bool RoleManager::initialize() {
    initializeBuiltinPermissions();

    if (!loadFromStorage()) {
        LOG_INFO("RoleManager: No role data found, creating builtin roles");
        initializeBuiltinRoles();
        saveToStorage();
    }

    char buf[56];
    snprintf(buf, sizeof(buf), "RoleManager: Initialized with %u roles", (unsigned)roles.size());
    LOG_INFO(buf);
    return true;
}

void RoleManager::initializeBuiltinPermissions() {
    using namespace BuiltinPermissions;
    auto reg = [&](const char* id, const char* name, const char* desc, const char* grp) {
        permDefs[id] = PermissionDef(id, name, desc, grp);
    };

    // 用户管理
    reg(USER_VIEW,    "查看用户",   "列出和查看用户信息",     "用户管理");
    reg(USER_CREATE,  "创建用户",   "新增用户账户",           "用户管理");
    reg(USER_EDIT,    "编辑用户",   "修改用户信息/密码/状态", "用户管理");
    reg(USER_DELETE,  "删除用户",   "移除用户账户",           "用户管理");

    // 角色管理
    reg(ROLE_VIEW,    "查看角色",   "列出和查看角色信息",     "角色管理");
    reg(ROLE_CREATE,  "创建角色",   "新增角色",               "角色管理");
    reg(ROLE_EDIT,    "编辑角色",   "修改角色名称/权限集合",  "角色管理");
    reg(ROLE_DELETE,  "删除角色",   "移除自定义角色",         "角色管理");

    // 系统
    reg(SYSTEM_VIEW,    "查看系统信息", "查看系统状态、健康信息", "系统");
    reg(SYSTEM_RESTART, "重启系统",     "触发设备重启",           "系统");

    // 配置
    reg(CONFIG_VIEW,  "查看配置",   "读取协议及系统配置",       "配置管理");
    reg(CONFIG_EDIT,  "编辑配置",   "修改协议及系统配置",       "配置管理");

    // 网络
    reg(NETWORK_VIEW, "查看网络",   "查看 WiFi 状态和配置",   "网络管理");
    reg(NETWORK_EDIT, "编辑网络",   "修改 WiFi 及 IP 配置",  "网络管理");

    // 设备
    reg(DEVICE_VIEW,    "查看设备",   "查看 GPIO 状态",         "设备控制");
    reg(DEVICE_CONTROL, "控制设备",   "操作 GPIO 输出",         "设备控制");

    // OTA
    reg(OTA_UPDATE,  "OTA 升级",    "上传并执行固件升级",       "OTA");

    // 文件系统
    reg(FS_VIEW,   "查看文件系统", "列出文件和目录",           "文件系统");
    reg(FS_MANAGE, "管理文件系统", "上传/删除文件",            "文件系统");

    // 审计
    reg(AUDIT_VIEW, "查看审计日志", "读取操作审计记录",        "审计");
}

void RoleManager::initializeBuiltinRoles() {
    using namespace BuiltinPermissions;

    // ── admin：拥有全部权限 ──
    Role admin(BuiltinRoles::ADMIN, "超级管理员", "拥有所有操作权限", true);
    for (const auto& kv : permDefs) {
        admin.permissions.push_back(kv.first);
    }
    roles[BuiltinRoles::ADMIN] = admin;

    // ── operator：设备操作 + 系统配置，不含用户/角色管理和文件系统管理 ──
    Role oper(BuiltinRoles::OPERATOR, "操作员", "可操作设备、修改配置、OTA升级，不能管理用户或文件系统", true);
    oper.permissions = {
        // 系统权限：查看 + 重启
        SYSTEM_VIEW, SYSTEM_RESTART,
        // 配置权限：查看 + 编辑
        CONFIG_VIEW, CONFIG_EDIT,
        // 网络权限：查看 + 编辑
        NETWORK_VIEW, NETWORK_EDIT,
        // 设备权限：查看 + 控制
        DEVICE_VIEW, DEVICE_CONTROL,
        // OTA 升级权限
        OTA_UPDATE,
        // 文件系统：仅查看
        FS_VIEW,
        // 审计查看
        AUDIT_VIEW,
        // 用户查看（不允许创建/编辑/删除）
        USER_VIEW,
        // 角色查看
        ROLE_VIEW
    };
    roles[BuiltinRoles::OPERATOR] = oper;

    // ── viewer：仅只读 ──
    Role viewer(BuiltinRoles::VIEWER, "查看者", "只读权限，不能修改任何配置", true);
    viewer.permissions = {
        SYSTEM_VIEW,
        CONFIG_VIEW,
        NETWORK_VIEW,
        DEVICE_VIEW,
        FS_VIEW,
        AUDIT_VIEW,
        USER_VIEW,
        ROLE_VIEW
    };
    roles[BuiltinRoles::VIEWER] = viewer;
}

// ─── 角色 CRUD ────────────────────────────────────────────────────────────────

bool RoleManager::createRole(const String& id, const String& name, const String& desc) {
    if (id.isEmpty() || name.isEmpty()) return false;
    if (roles.find(id) != roles.end()) {
        LOG_WARNING("RoleManager: Role already exists");
        return false;
    }
    roles[id] = Role(id, name, desc, false);
    return saveToStorage();
}

bool RoleManager::deleteRole(const String& id) {
    auto it = roles.find(id);
    if (it == roles.end()) return false;
    // 仅 admin 角色不可删除
    if (id == "admin") {
        LOG_WARNING("RoleManager: Cannot delete admin role");
        return false;
    }
    roles.erase(it);
    return saveToStorage();
}

bool RoleManager::updateRole(const String& id, const String& name, const String& desc) {
    auto it = roles.find(id);
    if (it == roles.end()) {
        LOG_WARNING("RoleManager::updateRole: Role not found: " + id);
        return false;
    }
    if (!name.isEmpty()) it->second.name = name;
    if (!desc.isEmpty())  it->second.description = desc;
    bool ok = saveToStorage();
    if (!ok) {
        LOG_ERROR("RoleManager::updateRole: saveToStorage failed for role: " + id);
    }
    return ok;
}

Role* RoleManager::getRole(const String& id) {
    auto it = roles.find(id);
    return (it != roles.end()) ? &it->second : nullptr;
}

std::vector<Role> RoleManager::getAllRoles() const {
    std::vector<Role> list;
    list.reserve(roles.size());
    for (const auto& kv : roles) {
        list.push_back(kv.second);
    }
    return list;
}

bool RoleManager::roleExists(const String& id) const {
    return roles.find(id) != roles.end();
}

// ─── 角色权限管理 ─────────────────────────────────────────────────────────────

bool RoleManager::grantPermission(const String& roleId, const String& permission) {
    auto it = roles.find(roleId);
    if (it == roles.end()) return false;
    // 去重
    if (it->second.hasPermission(permission)) return true;
    it->second.permissions.push_back(permission);
    return saveToStorage();
}

bool RoleManager::revokePermission(const String& roleId, const String& permission) {
    auto it = roles.find(roleId);
    if (it == roles.end()) return false;
    auto& perms = it->second.permissions;
    for (auto pit = perms.begin(); pit != perms.end(); ++pit) {
        if (*pit == permission) {
            perms.erase(pit);
            return saveToStorage();
        }
    }
    return false;
}

bool RoleManager::setRolePermissions(const String& roleId, const std::vector<String>& permissions) {
    auto it = roles.find(roleId);
    if (it == roles.end()) return false;
    it->second.permissions = permissions;
    return saveToStorage();
}

std::vector<String> RoleManager::getRolePermissions(const String& roleId) const {
    auto it = roles.find(roleId);
    return (it != roles.end()) ? it->second.permissions : std::vector<String>{};
}

bool RoleManager::roleHasPermission(const String& roleId, const String& permission) const {
    auto it = roles.find(roleId);
    if (it == roles.end()) return false;
    return it->second.hasPermission(permission);
}

// ─── 权限定义管理 ─────────────────────────────────────────────────────────────

void RoleManager::registerPermission(const PermissionDef& perm) {
    permDefs[perm.id] = perm;
}

std::vector<PermissionDef> RoleManager::getAllPermissions() const {
    std::vector<PermissionDef> list;
    list.reserve(permDefs.size());
    for (const auto& kv : permDefs) {
        list.push_back(kv.second);
    }
    return list;
}

std::map<String, std::vector<PermissionDef>> RoleManager::getPermissionsByGroup() const {
    std::map<String, std::vector<PermissionDef>> grouped;
    for (const auto& kv : permDefs) {
        grouped[kv.second.group].push_back(kv.second);
    }
    return grouped;
}

// ─── 序列化 ───────────────────────────────────────────────────────────────────

void RoleManager::roleToJsonObj(const Role& role, JsonObject& obj) {
    obj["id"]          = role.id;
    obj["name"]        = role.name;
    obj["description"] = role.description;
    obj["isBuiltin"]   = role.isBuiltin;
    JsonArray perms    = obj["permissions"].to<JsonArray>();
    for (const String& p : role.permissions) {
        perms.add(p);
    }
}

String RoleManager::toJson() const {
    JsonDocument doc;
    JsonArray arr = doc["roles"].to<JsonArray>();
    for (const auto& kv : roles) {
        JsonObject obj = arr.add<JsonObject>();
        roleToJsonObj(kv.second, obj);
    }
    String out;
    serializeJson(doc, out);
    return out;
}

String RoleManager::roleToJson(const String& id) const {
    auto it = roles.find(id);
    if (it == roles.end()) return "{}";
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    roleToJsonObj(it->second, obj);
    String out;
    serializeJson(doc, out);
    return out;
}

// ─── 持久化 ───────────────────────────────────────────────────────────────────

bool RoleManager::saveToStorage() {
    Serial.println("[RoleManager] saveToStorage called");
    
    JsonDocument doc;
    JsonArray arr = doc["roles"].to<JsonArray>();
    for (const auto& kv : roles) {
        JsonObject obj = arr.add<JsonObject>();
        roleToJsonObj(kv.second, obj);
    }
    
    // 确保目录存在
    if (!LittleFS.exists("/config")) {
        Serial.println("[RoleManager] Creating /config directory");
        if (!LittleFS.mkdir("/config")) {
            Serial.println("[RoleManager] ERROR: Failed to create /config directory");
            return false;
        }
    }
    
    // 使用文件系统存储
    File file = LittleFS.open(ROLES_FILE, "w");
    if (!file) {
        Serial.println("[RoleManager] ERROR: Failed to open roles file for writing");
        return false;
    }
    
    size_t written = serializeJson(doc, file);
    file.close();
    
    if (written > 0) {
        Serial.printf("[RoleManager] Roles saved to file (%d bytes)\n", written);
        return true;
    } else {
        Serial.println("[RoleManager] ERROR: Failed to write roles to file");
        return false;
    }
}

bool RoleManager::loadFromStorage() {
    if (!LittleFS.exists(ROLES_FILE)) {
        LOG_INFO("RoleManager: Roles file not found");
        return false;
    }
    
    File file = LittleFS.open(ROLES_FILE, "r");
    if (!file) {
        LOG_ERROR("RoleManager: Failed to open roles file for reading");
        return false;
    }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    
    if (err) {
        char buf[72];
        snprintf(buf, sizeof(buf), "RoleManager: Parse error: %s", err.c_str());
        LOG_ERROR(buf);
        return false;
    }

    roles.clear();
    JsonArray arr = doc["roles"];
    for (JsonObject obj : arr) {
        Role role;
        role.id          = obj["id"].as<String>();
        role.name        = obj["name"].as<String>();
        role.description = obj["description"].as<String>();
        role.isBuiltin   = obj["isBuiltin"] | false;

        JsonArray perms = obj["permissions"];
        for (const auto& p : perms) {
            role.permissions.push_back(p.as<String>());
        }
        // 确保内置角色标记正确（防止持久化数据被篡改）
        if (role.id == BuiltinRoles::ADMIN ||
            role.id == BuiltinRoles::OPERATOR ||
            role.id == BuiltinRoles::VIEWER) {
            role.isBuiltin = true;
        }
        roles[role.id] = role;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "RoleManager: Loaded %u roles from storage", (unsigned)roles.size());
    LOG_INFO(buf);
    return true;
}
