#pragma once
struct RfidTestLogger {
    template<class... T> void info(T...) {}
    template<class... T> void warning(T...) {}
    template<class... T> void errorf(T...) {}
    template<class... T> void infof(T...) {}
};
inline RfidTestLogger LOGGER;
