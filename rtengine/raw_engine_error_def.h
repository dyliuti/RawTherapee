#ifndef RAW_ENGINE_ERROR_DEF_H
#define RAW_ENGINE_ERROR_DEF_H

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum {
    RawEngineErrorNone = 0,
    RawEngineErrorLoadFail = 99999999,
} RawEngineError;

#ifdef __cplusplus
}

#endif


#endif // RAW_ENGINE_ERROR_DEF_H
