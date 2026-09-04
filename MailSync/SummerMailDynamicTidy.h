//
//  SummerMailDynamicTidy.h
//  SummerMail-Sync
//
//  Dynamic loading wrapper for libtidy on Linux.
//  This allows the same binary to work across different Linux distributions
//  that use different sonames for libtidy (Debian: libtidy.so.5deb1,
//  Fedora: libtidy.so.5, etc.)
//

#ifndef SUMMERMAIL_DYNAMIC_TIDY_H
#define SUMMERMAIL_DYNAMIC_TIDY_H

#if defined(__linux__)

#ifdef __cplusplus
extern "C" {
#endif

// Opaque tidy types - we only need pointers
typedef void* MSTidyDoc;
typedef struct _MSTidyBuffer {
    void* allocator;
    unsigned char* bp;
    unsigned int size;
    unsigned int allocated;
    unsigned int next;
} MSTidyBuffer;

// Boolean type matching libtidy
typedef enum { MSTidyNo = 0, MSTidyYes = 1 } MSTidyBool;

// Doctype mode value (TidyDoctypeUser from TidyDoctypeModes enum)
// In libtidy 5.x: Html5=0, Omit=1, Auto=2, Strict=3, Loose=4, User=5
#define MSTidyDoctypeUser 5

// Initialize libtidy dynamic loading - called automatically at startup
void summermail_tidy_init(void);

// Check if libtidy is available
int summermail_tidy_available(void);

// Get error message if tidy loading failed (returns NULL if available)
const char* summermail_tidy_error(void);

// Wrapper functions matching libtidy API
MSTidyDoc summermail_tidyCreate(void);
void summermail_tidyRelease(MSTidyDoc tdoc);
void summermail_tidyBufInit(MSTidyBuffer* buf);
void summermail_tidyBufFree(MSTidyBuffer* buf);
void summermail_tidyBufAppend(MSTidyBuffer* buf, void* data, unsigned int size);
MSTidyBool summermail_tidyOptSetBool(MSTidyDoc tdoc, unsigned int optId, MSTidyBool val);
MSTidyBool summermail_tidyOptSetInt(MSTidyDoc tdoc, unsigned int optId, unsigned long val);
int summermail_tidySetCharEncoding(MSTidyDoc tdoc, const char* encnam);
int summermail_tidySetErrorBuffer(MSTidyDoc tdoc, MSTidyBuffer* errbuf);
int summermail_tidyParseBuffer(MSTidyDoc tdoc, MSTidyBuffer* buf);
int summermail_tidyCleanAndRepair(MSTidyDoc tdoc);
int summermail_tidySaveBuffer(MSTidyDoc tdoc, MSTidyBuffer* buf);

// Getters for dynamically resolved option IDs (looked up by name at init)
unsigned int summermail_tidyOptId_XhtmlOut(void);
unsigned int summermail_tidyOptId_DoctypeMode(void);
unsigned int summermail_tidyOptId_Mark(void);
unsigned int summermail_tidyOptId_ForceOutput(void);
unsigned int summermail_tidyOptId_ShowWarnings(void);
unsigned int summermail_tidyOptId_ShowErrors(void);

#ifdef __cplusplus
}
#endif

#endif // __linux__

#endif // SUMMERMAIL_DYNAMIC_TIDY_H
