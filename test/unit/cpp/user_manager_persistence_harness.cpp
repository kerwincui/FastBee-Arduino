#include <Arduino.h>
#include <cassert>
#include <map>
#define FASTBEE_SINGLE_ADMIN_MODE 0
static const char* DEFAULT_ADMIN_USER="admin";
struct User { String username,salt,passwordHash,description;bool enabled=true;uint32_t createTime=0,lastModified=0; };
struct UsersLock { explicit UsersLock(void*) {} };
struct UserManager {
 void* _usersMutex=nullptr;
 std::map<String,User> users;
 std::map<String,uint8_t> loginAttempts;
 struct Config { uint32_t sessionTimeout=10,sessionCleanupInterval=2,cookieMaxAge=15;bool enableSessionPersistence=true,cookieHttpOnly=true,cookieSecure=false;String cookieName="original"; } config;
 bool failing=false;int writes=0;
 bool saveUsersToStorage(){++writes;return !failing;}
 bool validatePasswordStrength(const String& password){return password.length()>=4;}
 String generateSalt(){return "salt-new";}
 String hashPasswordPbkdf2(const String& password){return "hash:"+password;}
 bool verifyPassword(const String& password,const String& hash,const String&){return hash==hashPasswordPbkdf2(password);}
 bool addUser(const String&,const String&,const String& = "");
 bool deleteUser(const String&);
 bool updateUser(const String&,const String&,bool,const String* = nullptr);
 bool changePassword(const String&,const String&,const String&);
 bool resetPassword(const String&,const String&);
 bool setUserDescription(const String&,const String&);
 bool updateSecurityConfig(uint32_t,uint32_t,bool,const String&,uint32_t,bool,bool);
};
#include "user_save.inc"
int main(){
 UserManager m;assert(m.addUser("operator","before","original"));assert(m.writes==1);assert(m.users["operator"].description=="original");const User previous=m.users["operator"];
 m.failing=true;assert(!m.addUser("new","valid"));assert(m.users.count("new")==0);
 auto unchanged=[&](){const auto& u=m.users["operator"];assert(u.passwordHash==previous.passwordHash&&u.salt==previous.salt&&u.enabled==previous.enabled&&u.lastModified==previous.lastModified&&u.description==previous.description);};
 String description="changed";int before=m.writes;
 assert(!m.updateUser("operator","after",false,&description));unchanged();assert(m.writes==before+1);
 assert(!m.updateUser("operator","bad",false,&description));unchanged();assert(m.writes==before+1);
 assert(!m.changePassword("operator","before","after"));unchanged();
 assert(!m.resetPassword("operator","after"));unchanged();
 assert(!m.setUserDescription("operator","new-description"));unchanged();
 m.loginAttempts["operator"]=3;assert(!m.deleteUser("operator"));unchanged();assert(m.loginAttempts["operator"]==3);
 assert(!m.updateSecurityConfig(30,8,false,"changed",32,false,true));assert(m.config.sessionTimeout==10&&m.config.cookieName=="original"&&m.config.enableSessionPersistence);
 m.failing=false;before=m.writes;assert(m.updateUser("operator","after",false,&description));assert(m.writes==before+1);assert(m.users["operator"].description=="changed");assert(!m.users["operator"].enabled&&m.users["operator"].passwordHash=="hash:after");
 description="";assert(m.updateUser("operator","",false,&description));assert(m.users["operator"].description.isEmpty());
 assert(m.changePassword("operator","after","next"));assert(m.resetPassword("operator","final"));
 assert(m.setUserDescription("operator","description"));assert(m.users["operator"].description=="description");
 assert(m.updateSecurityConfig(30,8,false,"changed",32,false,true));assert(m.config.cookieName=="changed"&&m.config.cookieSecure);
 assert(m.deleteUser("operator"));assert(!m.users.count("operator")&&!m.loginAttempts.count("operator"));
 assert(!m.deleteUser("admin"));assert(!m.addUser("invalid","bad"));return 0;
}

