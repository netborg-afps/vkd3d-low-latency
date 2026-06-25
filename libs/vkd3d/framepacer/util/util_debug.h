#pragma once
#include "util_log.h"

class Debug {
public:
    Debug( const char* msg ) : m_msg(msg) { INFO( "%s start \n", m_msg ); }
    ~Debug() { INFO( "%s done \n", m_msg ); }
    const char* m_msg;
};
