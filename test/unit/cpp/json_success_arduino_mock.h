#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>
inline size_t stringBytes=0,stringPeak=0;
inline bool failReserve=false,failArray=false,failResponse=false;
inline int failConcatAfter=-1;
class String {
    std::string value_;
    size_t capacity_=0;
    void release() {stringBytes-=capacity_;capacity_=0;}
public:
    String()=default;
    String(const char* text) {if(text){reserve(strlen(text));value_=text;}}
    String(unsigned long n):String(std::to_string(n).c_str()) {}
    String(const String& other):String(other.c_str()) {}
    String(String&& other) noexcept:value_(std::move(other.value_)),capacity_(other.capacity_) {other.capacity_=0;}
    ~String() {release();}
    String& operator=(const char* text) {value_.clear();if(text){reserve(strlen(text));value_=text;}return *this;}
    String& operator=(String&& other) noexcept {
        release();value_=std::move(other.value_);capacity_=other.capacity_;other.capacity_=0;return *this;
    }
    bool reserve(size_t size) {
        if(failReserve)return false;
        if(size+1>capacity_) {stringBytes+=size+1-capacity_;capacity_=size+1;stringPeak=std::max(stringPeak,stringBytes);}
        value_.reserve(size);return true;
    }
    bool concat(const char* data,size_t count) {
        if(failConcatAfter==0)return false;
        if(failConcatAfter>0)--failConcatAfter;
        if(!reserve(value_.size()+count))return false;
        value_.append(data,count);return true;
    }
    String& operator+=(const char* text) {concat(text,strlen(text));return *this;}
    String& operator+=(const String& text) {concat(text.c_str(),text.length());return *this;}
    size_t write(uint8_t byte) {return concat(reinterpret_cast<char*>(&byte),1)?1:0;}
    size_t write(const uint8_t* data,size_t count) {return concat(reinterpret_cast<const char*>(data),count)?count:0;}
    size_t length() const {return value_.size();}
    const char* c_str() const {return value_.c_str();}
    static void* operator new[](size_t size,const std::nothrow_t&) noexcept {
        return failArray?nullptr: ::operator new[](size,std::nothrow);
    }
    static void operator delete[](void* memory) noexcept {::operator delete[](memory);}
    static void operator delete[](void* memory,const std::nothrow_t&) noexcept {::operator delete[](memory);}
};
inline unsigned long millis() {return 4294967295UL;}
