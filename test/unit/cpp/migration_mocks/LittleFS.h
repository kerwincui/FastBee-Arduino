#pragma once
#include <Arduino.h>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <algorithm>
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
struct MigrationPowerLoss {};
struct File;
struct MigrationFileSystem {
    std::map<std::string,std::string> files;
    std::set<std::string> directories;
    int powerAfter=-1;
    unsigned mutations=0;
    size_t capacity=1024*1024,writeLimit=SIZE_MAX;
    std::string failRenameTo,failRemove;
    void changed(){++mutations;if(powerAfter==0)throw MigrationPowerLoss{};if(powerAfter>0)--powerAfter;}
    bool exists(const String& path)const{return files.count(std::string(path))||directories.count(std::string(path));}
    bool mkdir(const String& path){directories.insert(std::string(path));changed();return true;}
    bool remove(const String& path){
        if(std::string(path)==failRemove)return false;
        if(!files.erase(std::string(path)))return false;
        changed();return true;
    }
    bool rmdir(const String& path){
        const std::string prefix=std::string(path)+"/";
        for(const auto& file:files)if(file.first.compare(0,prefix.size(),prefix)==0)return false;
        if(!directories.erase(std::string(path)))return false;
        changed();return true;
    }
    bool rename(const String& from,const String& to){
        const std::string source(from),target(to);
        if(target==failRenameTo)return false;
        if(directories.count(source)){
            if(exists(to))return false;
            const std::string prefix=source+"/";
            auto original=files;
            for(const auto& file:original)if(file.first.compare(0,prefix.size(),prefix)==0){
                files[target+file.first.substr(source.size())]=file.second;files.erase(file.first);
            }
            directories.erase(source);directories.insert(target);changed();return true;
        }
        const auto file=files.find(source);if(file==files.end())return false;
        const auto contents=file->second;files.erase(file);files[target]=contents;changed();return true;
    }
    size_t totalBytes()const{return capacity;}
    size_t usedBytes()const{size_t total=0;for(const auto& f:files)total+=f.second.size();return total;}
    File open(const String& path,const char* mode);
};
inline MigrationFileSystem LittleFS;
struct File {
    std::string path;
    bool valid=false,writable=false,directory=false;
    size_t offset=0,childIndex=0;
    explicit operator bool()const{return valid;}
    bool isDirectory()const{return directory;}
    const char* name()const{return path.c_str();}
    size_t size()const{return valid&&!directory?LittleFS.files.at(path).size():0;}
    int available()const{return offset<size()?int(size()-offset):0;}
    int read(){if(!valid||offset>=size())return -1;return static_cast<unsigned char>(LittleFS.files.at(path)[offset++]);}
    size_t read(uint8_t* out,size_t count){size_t n=0;for(;n<count;++n){int value=read();if(value<0)break;out[n]=uint8_t(value);}return n;}
    bool seek(size_t position){if(!valid||position>size())return false;offset=position;return true;}
    size_t write(const uint8_t* bytes,size_t count){
        if(!valid||!writable)return 0;
        count=std::min(count,LittleFS.writeLimit);
        auto& target=LittleFS.files[path];target.append(reinterpret_cast<const char*>(bytes),count);offset=target.size();
        LittleFS.changed();return count;
    }
    void flush(){}
    void close(){valid=false;}
    File openNextFile(){
        if(!valid||!directory)return {};
        const auto prefix=path+"/";size_t index=0;
        for(const auto& row:LittleFS.files)if(row.first.compare(0,prefix.size(),prefix)==0){
            if(index++==childIndex){++childIndex;return {row.first,true,false,false,0,0};}
        }return {};
    }
};
inline File MigrationFileSystem::open(const String& path,const char* mode){
    const std::string key(path);
    if(directories.count(key))return {key,true,false,true,0,0};
    const bool write=mode[0]=='w'||mode[0]=='a';
    if(write){if(mode[0]=='w'||!files.count(key)){files[key].clear();changed();}}
    else if(!files.count(key))return {};
    return {key,true,write,false,mode[0]=='a'?files[key].size():0,0};
}
