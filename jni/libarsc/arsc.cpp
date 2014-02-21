/*
 * arsc.cpp
 *
 *  Created on: 2014-2-18
 *      Author: auxten
 */

#include <jni.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#include <dlfcn.h>
#include <iostream>
#include <ctype.h>
#include <set>
#include <dirent.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/un.h>
#include <stdint.h>

#include "arsc.h"
#include "Errors.h"
#include "Vector.h"

using namespace android;

#ifndef INT32_MAX
#define INT32_MAX ((int32_t)(2147483647))
#endif

//#define POOL_NOISY(x) //x
//#define XML_NOISY(x) //x
//#define TABLE_NOISY(x) //x
#define TABLE_GETENTRY(x) //x
//#define TABLE_SUPER_NOISY(x) //x
#define LOAD_TABLE_NOISY(x) //x
//#define TABLE_THEME(x) //x


void print_complex(uint32_t complex, bool isFraction)
{
    const float MANTISSA_MULT =
        1.0f / (1<<Res_value::COMPLEX_MANTISSA_SHIFT);
    const float RADIX_MULTS[] = {
        1.0f*MANTISSA_MULT, 1.0f/(1<<7)*MANTISSA_MULT,
        1.0f/(1<<15)*MANTISSA_MULT, 1.0f/(1<<23)*MANTISSA_MULT
    };

    float value = (complex&(Res_value::COMPLEX_MANTISSA_MASK
                   <<Res_value::COMPLEX_MANTISSA_SHIFT))
            * RADIX_MULTS[(complex>>Res_value::COMPLEX_RADIX_SHIFT)
                            & Res_value::COMPLEX_RADIX_MASK];
    LOGD("%f", value);

    if (!isFraction) {
        switch ((complex>>Res_value::COMPLEX_UNIT_SHIFT)&Res_value::COMPLEX_UNIT_MASK) {
            case Res_value::COMPLEX_UNIT_PX: LOGD("px"); break;
            case Res_value::COMPLEX_UNIT_DIP: LOGD("dp"); break;
            case Res_value::COMPLEX_UNIT_SP: LOGD("sp"); break;
            case Res_value::COMPLEX_UNIT_PT: LOGD("pt"); break;
            case Res_value::COMPLEX_UNIT_IN: LOGD("in"); break;
            case Res_value::COMPLEX_UNIT_MM: LOGD("mm"); break;
            default: LOGD(" (unknown unit)"); break;
        }
    } else {
        switch ((complex>>Res_value::COMPLEX_UNIT_SHIFT)&Res_value::COMPLEX_UNIT_MASK) {
            case Res_value::COMPLEX_UNIT_FRACTION: LOGD("%%"); break;
            case Res_value::COMPLEX_UNIT_FRACTION_PARENT: LOGD("%%p"); break;
            default: LOGD(" (unknown unit)"); break;
        }
    }
}

// Normalize a string for output
String8 ResTable::normalizeForOutput( const char *input )
{
    String8 ret;
    char buff[2];
    buff[1] = '\0';

    while (*input != '\0') {
        switch (*input) {
            // All interesting characters are in the ASCII zone, so we are making our own lives
            // easier by scanning the string one byte at a time.
        case '\\':
            ret += "\\\\";
            break;
        case '\n':
            ret += "\\n";
            break;
        case '"':
            ret += "\\\"";
            break;
        default:
            buff[0] = *input;
            ret += buff;
            break;
        }

        input++;
    }

    return ret;
}

// --------------------------------------------------------------------
// --------------------------------------------------------------------
// --------------------------------------------------------------------

ResStringPool::ResStringPool()
    : mError(NO_INIT), mOwnedData(NULL), mHeader(NULL), mCache(NULL)
{
}

ResStringPool::ResStringPool(const void* data, size_t size, bool copyData)
    : mError(NO_INIT), mOwnedData(NULL), mHeader(NULL), mCache(NULL)
{
    setTo(data, size, copyData);
}

ResStringPool::~ResStringPool()
{
    uninit();
}

status_t ResStringPool::setTo(const void* data, size_t size, bool copyData)
{
    if (!data || !size) {
        return (mError=BAD_TYPE);
    }

    uninit();

    const bool notDeviceEndian = htods(0xf0) != 0xf0;

    if (copyData || notDeviceEndian) {
        mOwnedData = malloc(size);
        if (mOwnedData == NULL) {
            return (mError=NO_MEMORY);
        }
        memcpy(mOwnedData, data, size);
        data = mOwnedData;
    }

    mHeader = (const ResStringPool_header*)data;

    if (notDeviceEndian) {
        ResStringPool_header* h = const_cast<ResStringPool_header*>(mHeader);
        h->header.headerSize = dtohs(mHeader->header.headerSize);
        h->header.type = dtohs(mHeader->header.type);
        h->header.size = dtohl(mHeader->header.size);
        h->stringCount = dtohl(mHeader->stringCount);
        h->styleCount = dtohl(mHeader->styleCount);
        h->flags = dtohl(mHeader->flags);
        h->stringsStart = dtohl(mHeader->stringsStart);
        h->stylesStart = dtohl(mHeader->stylesStart);
    }

    if (mHeader->header.headerSize > mHeader->header.size
            || mHeader->header.size > size) {
        LOGW("Bad string block: header size %d or total size %d is larger than data size %d\n",
                (int)mHeader->header.headerSize, (int)mHeader->header.size, (int)size);
        return (mError=BAD_TYPE);
    }
    mSize = mHeader->header.size;
    mEntries = (const uint32_t*)
        (((const uint8_t*)data)+mHeader->header.headerSize);

    if (mHeader->stringCount > 0) {
        if ((mHeader->stringCount*sizeof(uint32_t) < mHeader->stringCount)  // uint32 overflow?
            || (mHeader->header.headerSize+(mHeader->stringCount*sizeof(uint32_t)))
                > size) {
            LOGW("Bad string block: entry of %d items extends past data size %d\n",
                    (int)(mHeader->header.headerSize+(mHeader->stringCount*sizeof(uint32_t))),
                    (int)size);
            return (mError=BAD_TYPE);
        }

        size_t charSize;
        if (mHeader->flags&ResStringPool_header::UTF8_FLAG) {
            charSize = sizeof(uint8_t);
            mCache = (char16_t**)malloc(sizeof(char16_t**)*mHeader->stringCount);
            memset(mCache, 0, sizeof(char16_t**)*mHeader->stringCount);
        } else {
            charSize = sizeof(char16_t);
        }

        mStrings = (const void*)
            (((const uint8_t*)data)+mHeader->stringsStart);
        if (mHeader->stringsStart >= (mHeader->header.size-sizeof(uint16_t))) {
            LOGW("Bad string block: string pool starts at %d, after total size %d\n",
                    (int)mHeader->stringsStart, (int)mHeader->header.size);
            return (mError=BAD_TYPE);
        }
        if (mHeader->styleCount == 0) {
            mStringPoolSize =
                (mHeader->header.size-mHeader->stringsStart)/charSize;
        } else {
            // check invariant: styles starts before end of data
            if (mHeader->stylesStart >= (mHeader->header.size-sizeof(uint16_t))) {
                LOGW("Bad style block: style block starts at %d past data size of %d\n",
                    (int)mHeader->stylesStart, (int)mHeader->header.size);
                return (mError=BAD_TYPE);
            }
            // check invariant: styles follow the strings
            if (mHeader->stylesStart <= mHeader->stringsStart) {
                LOGW("Bad style block: style block starts at %d, before strings at %d\n",
                    (int)mHeader->stylesStart, (int)mHeader->stringsStart);
                return (mError=BAD_TYPE);
            }
            mStringPoolSize =
                (mHeader->stylesStart-mHeader->stringsStart)/charSize;
        }

        // check invariant: stringCount > 0 requires a string pool to exist
        if (mStringPoolSize == 0) {
            LOGW("Bad string block: stringCount is %d but pool size is 0\n", (int)mHeader->stringCount);
            return (mError=BAD_TYPE);
        }

        if (notDeviceEndian) {
            size_t i;
            uint32_t* e = const_cast<uint32_t*>(mEntries);
            for (i=0; i<mHeader->stringCount; i++) {
                e[i] = dtohl(mEntries[i]);
            }
            if (!(mHeader->flags&ResStringPool_header::UTF8_FLAG)) {
                const char16_t* strings = (const char16_t*)mStrings;
                char16_t* s = const_cast<char16_t*>(strings);
                for (i=0; i<mStringPoolSize; i++) {
                    s[i] = dtohs(strings[i]);
                }
            }
        }

        if ((mHeader->flags&ResStringPool_header::UTF8_FLAG &&
                ((uint8_t*)mStrings)[mStringPoolSize-1] != 0) ||
                (!mHeader->flags&ResStringPool_header::UTF8_FLAG &&
                ((char16_t*)mStrings)[mStringPoolSize-1] != 0)) {
            LOGW("Bad string block: last string is not 0-terminated\n");
            return (mError=BAD_TYPE);
        }
    } else {
        mStrings = NULL;
        mStringPoolSize = 0;
    }

    if (mHeader->styleCount > 0) {
        mEntryStyles = mEntries + mHeader->stringCount;
        // invariant: integer overflow in calculating mEntryStyles
        if (mEntryStyles < mEntries) {
            LOGW("Bad string block: integer overflow finding styles\n");
            return (mError=BAD_TYPE);
        }

        if (((const uint8_t*)mEntryStyles-(const uint8_t*)mHeader) > (int)size) {
            LOGW("Bad string block: entry of %d styles extends past data size %d\n",
                    (int)((const uint8_t*)mEntryStyles-(const uint8_t*)mHeader),
                    (int)size);
            return (mError=BAD_TYPE);
        }
        mStyles = (const uint32_t*)
            (((const uint8_t*)data)+mHeader->stylesStart);
        if (mHeader->stylesStart >= mHeader->header.size) {
            LOGW("Bad string block: style pool starts %d, after total size %d\n",
                    (int)mHeader->stylesStart, (int)mHeader->header.size);
            return (mError=BAD_TYPE);
        }
        mStylePoolSize =
            (mHeader->header.size-mHeader->stylesStart)/sizeof(uint32_t);

        if (notDeviceEndian) {
            size_t i;
            uint32_t* e = const_cast<uint32_t*>(mEntryStyles);
            for (i=0; i<mHeader->styleCount; i++) {
                e[i] = dtohl(mEntryStyles[i]);
            }
            uint32_t* s = const_cast<uint32_t*>(mStyles);
            for (i=0; i<mStylePoolSize; i++) {
                s[i] = dtohl(mStyles[i]);
            }
        }

        const ResStringPool_span endSpan = {
            { htodl(ResStringPool_span::END) },
            htodl(ResStringPool_span::END), htodl(ResStringPool_span::END)
        };
        if (memcmp(&mStyles[mStylePoolSize-(sizeof(endSpan)/sizeof(uint32_t))],
                   &endSpan, sizeof(endSpan)) != 0) {
            LOGW("Bad string block: last style is not 0xFFFFFFFF-terminated\n");
            return (mError=BAD_TYPE);
        }
    } else {
        mEntryStyles = NULL;
        mStyles = NULL;
        mStylePoolSize = 0;
    }

    return (mError=NO_ERROR);
}

status_t ResStringPool::getError() const
{
    return mError;
}

void ResStringPool::uninit()
{
    mError = NO_INIT;
    if (mOwnedData) {
        free(mOwnedData);
        mOwnedData = NULL;
    }
    if (mHeader != NULL && mCache != NULL) {
        for (size_t x = 0; x < mHeader->stringCount; x++) {
            if (mCache[x] != NULL) {
                free(mCache[x]);
                mCache[x] = NULL;
            }
        }
        free(mCache);
        mCache = NULL;
    }
}

/**
 * Strings in UTF-16 format have length indicated by a length encoded in the
 * stored data. It is either 1 or 2 characters of length data. This allows a
 * maximum length of 0x7FFFFFF (2147483647 bytes), but if you're storing that
 * much data in a string, you're abusing them.
 *
 * If the high bit is set, then there are two characters or 4 bytes of length
 * data encoded. In that case, drop the high bit of the first character and
 * add it together with the next character.
 */
static inline size_t
decodeLength(const char16_t** str)
{
    size_t len = **str;
    if ((len & 0x8000) != 0) {
        (*str)++;
        len = ((len & 0x7FFF) << 16) | **str;
    }
    (*str)++;
    return len;
}

/**
 * Strings in UTF-8 format have length indicated by a length encoded in the
 * stored data. It is either 1 or 2 characters of length data. This allows a
 * maximum length of 0x7FFF (32767 bytes), but you should consider storing
 * text in another way if you're using that much data in a single string.
 *
 * If the high bit is set, then there are two characters or 2 bytes of length
 * data encoded. In that case, drop the high bit of the first character and
 * add it together with the next character.
 */
static inline size_t
decodeLength(const uint8_t** str)
{
    size_t len = **str;
    if ((len & 0x80) != 0) {
        (*str)++;
        len = ((len & 0x7F) << 8) | **str;
    }
    (*str)++;
    return len;
}

const uint16_t* ResStringPool::stringAt(size_t idx, size_t* u16len) const
{
    if (mError == NO_ERROR && idx < mHeader->stringCount) {
        const bool isUTF8 = (mHeader->flags&ResStringPool_header::UTF8_FLAG) != 0;
        const uint32_t off = mEntries[idx]/(isUTF8?sizeof(char):sizeof(char16_t));
        if (off < (mStringPoolSize-1)) {
            if (!isUTF8) {
                const char16_t* strings = (char16_t*)mStrings;
                const char16_t* str = strings+off;

                *u16len = decodeLength(&str);
                if ((uint32_t)(str+*u16len-strings) < mStringPoolSize) {
                    return str;
                } else {
                    LOGW("Bad string block: string #%d extends to %d, past end at %d\n",
                            (int)idx, (int)(str+*u16len-strings), (int)mStringPoolSize);
                }
            } else {
                const uint8_t* strings = (uint8_t*)mStrings;
                const uint8_t* u8str = strings+off;

                *u16len = decodeLength(&u8str);
                size_t u8len = decodeLength(&u8str);

                // encLen must be less than 0x7FFF due to encoding.
                if ((uint32_t)(u8str+u8len-strings) < mStringPoolSize) {
//                    AutoMutex lock(mDecodeLock);

                    if (mCache[idx] != NULL) {
                        return mCache[idx];
                    }

                    ssize_t actualLen = utf8_to_utf16_length(u8str, u8len);
                    if (actualLen < 0 || (size_t)actualLen != *u16len) {
                        LOGW("Bad string block: string #%lld decoded length is not correct "
                                "%lld vs %llu\n",
                                (long long)idx, (long long)actualLen, (long long)*u16len);
                        return NULL;
                    }

                    char16_t *u16str = (char16_t *)calloc(*u16len+1, sizeof(char16_t));
                    if (!u16str) {
                        LOGW("No memory when trying to allocate decode cache for string #%d\n",
                                (int)idx);
                        return NULL;
                    }

                    utf8_to_utf16(u8str, u8len, u16str);
                    mCache[idx] = u16str;
                    return u16str;
                } else {
                    LOGW("Bad string block: string #%lld extends to %lld, past end at %lld\n",
                            (long long)idx, (long long)(u8str+u8len-strings),
                            (long long)mStringPoolSize);
                }
            }
        } else {
            LOGW("Bad string block: string #%d entry is at %d, past end at %d\n",
                    (int)idx, (int)(off*sizeof(uint16_t)),
                    (int)(mStringPoolSize*sizeof(uint16_t)));
        }
    }
    return NULL;
}

const char* ResStringPool::string8At(size_t idx, size_t* outLen) const
{
    if (mError == NO_ERROR && idx < mHeader->stringCount) {
        const bool isUTF8 = (mHeader->flags&ResStringPool_header::UTF8_FLAG) != 0;
        const uint32_t off = mEntries[idx]/(isUTF8?sizeof(char):sizeof(char16_t));
        if (off < (mStringPoolSize-1)) {
            if (isUTF8) {
                const uint8_t* strings = (uint8_t*)mStrings;
                const uint8_t* str = strings+off;
                *outLen = decodeLength(&str);
                size_t encLen = decodeLength(&str);
                if ((uint32_t)(str+encLen-strings) < mStringPoolSize) {
                    return (const char*)str;
                } else {
                    LOGW("Bad string block: string #%d extends to %d, past end at %d\n",
                            (int)idx, (int)(str+encLen-strings), (int)mStringPoolSize);
                }
            }
        } else {
            LOGW("Bad string block: string #%d entry is at %d, past end at %d\n",
                    (int)idx, (int)(off*sizeof(uint16_t)),
                    (int)(mStringPoolSize*sizeof(uint16_t)));
        }
    }
    return NULL;
}

const ResStringPool_span* ResStringPool::styleAt(const ResStringPool_ref& ref) const
{
    return styleAt(ref.index);
}

const ResStringPool_span* ResStringPool::styleAt(size_t idx) const
{
    if (mError == NO_ERROR && idx < mHeader->styleCount) {
        const uint32_t off = (mEntryStyles[idx]/sizeof(uint32_t));
        if (off < mStylePoolSize) {
            return (const ResStringPool_span*)(mStyles+off);
        } else {
            LOGW("Bad string block: style #%d entry is at %d, past end at %d\n",
                    (int)idx, (int)(off*sizeof(uint32_t)),
                    (int)(mStylePoolSize*sizeof(uint32_t)));
        }
    }
    return NULL;
}

ssize_t ResStringPool::indexOfString(const char16_t* str, size_t strLen) const
{
    if (mError != NO_ERROR) {
        return mError;
    }

    size_t len;

    // TODO optimize searching for UTF-8 strings taking into account
    // the cache fill to determine when to convert the searched-for
    // string key to UTF-8.

    if (mHeader->flags&ResStringPool_header::SORTED_FLAG) {
        // Do a binary search for the string...
        ssize_t l = 0;
        ssize_t h = mHeader->stringCount-1;

        ssize_t mid;
        while (l <= h) {
            mid = l + (h - l)/2;
            const char16_t* s = stringAt(mid, &len);
            int c = s ? strzcmp16(s, len, str, strLen) : -1;
            POOL_NOISY(LOGD("Looking for %s, at %s, cmp=%d, l/mid/h=%d/%d/%d\n",
                         String8(str).string(),
                         String8(s).string(),
                         c, (int)l, (int)mid, (int)h));
            if (c == 0) {
                return mid;
            } else if (c < 0) {
                l = mid + 1;
            } else {
                h = mid - 1;
            }
        }
    } else {
        // It is unusual to get the ID from an unsorted string block...
        // most often this happens because we want to get IDs for style
        // span tags; since those always appear at the end of the string
        // block, start searching at the back.
        for (int i=mHeader->stringCount-1; i>=0; i--) {
            const char16_t* s = stringAt(i, &len);
            POOL_NOISY(LOGD("Looking for %s, at %s, i=%d\n",
                         String8(str, strLen).string(),
                         String8(s).string(),
                         i));
            if (s && strzcmp16(s, len, str, strLen) == 0) {
                return i;
            }
        }
    }

    return NAME_NOT_FOUND;
}

size_t ResStringPool::size() const
{
    return (mError == NO_ERROR) ? mHeader->stringCount : 0;
}

#ifndef HAVE_ANDROID_OS
bool ResStringPool::isUTF8() const
{
    return (mHeader->flags&ResStringPool_header::UTF8_FLAG)!=0;
}
#endif


struct ResTable::Header
{
    Header(ResTable* _owner) : owner(_owner), ownedData(NULL), header(NULL),
        resourceIDMap(NULL), resourceIDMapSize(0) { }

    ~Header()
    {
        free(resourceIDMap);
    }

    ResTable* const                 owner;
    void*                           ownedData;
    const ResTable_header*          header;
    size_t                          size;
    const uint8_t*                  dataEnd;
    size_t                          index;
    void*                           cookie;

    ResStringPool                   values;
    uint32_t*                       resourceIDMap;
    size_t                          resourceIDMapSize;
};

struct ResTable::Type
{
    Type(const Header* _header, const Package* _package, size_t count)
        : header(_header), package(_package), entryCount(count),
          typeSpec(NULL), typeSpecFlags(NULL) { }
    const Header* const             header;
    const Package* const            package;
    const size_t                    entryCount;
    const ResTable_typeSpec*        typeSpec;
    const uint32_t*                 typeSpecFlags;
    Vector<const ResTable_type*>    configs;
};

struct ResTable::Package
{
    Package(ResTable* _owner, const Header* _header, const ResTable_package* _package)
        : owner(_owner), header(_header), package(_package) { }
    ~Package()
    {
        size_t i = types.size();
        while (i > 0) {
            i--;
            delete types[i];
        }
    }

    ResTable* const                 owner;
    const Header* const             header;
    const ResTable_package* const   package;
    Vector<Type*>                   types;

    ResStringPool                   typeStrings;
    ResStringPool                   keyStrings;

    const Type* getType(size_t idx) const {
        return idx < types.size() ? types[idx] : NULL;
    }
};

// A group of objects describing a particular resource package.
// The first in 'package' is always the root object (from the resource
// table that defined the package); the ones after are skins on top of it.
struct ResTable::PackageGroup
{
    PackageGroup(ResTable* _owner, const String16& _name, uint32_t _id)
        : owner(_owner), name(_name), id(_id), typeCount(0), bags(NULL) { }
    ~PackageGroup() {
        clearBagCache();
        const size_t N = packages.size();
        for (size_t i=0; i<N; i++) {
            Package* pkg = packages[i];
            if (pkg->owner == owner) {
                delete pkg;
            }
        }
    }

    void clearBagCache() {
        if (bags) {
            TABLE_NOISY(LOGD("bags=%p\n", bags));
            Package* pkg = packages[0];
            TABLE_NOISY(LOGD("typeCount=%x\n", typeCount));
            for (size_t i=0; i<typeCount; i++) {
                TABLE_NOISY(LOGD("type=%d\n", i));
                const Type* type = pkg->getType(i);
                if (type != NULL) {
                    bag_set** typeBags = bags[i];
                    TABLE_NOISY(LOGD("typeBags=%p\n", typeBags));
                    if (typeBags) {
                        TABLE_NOISY(LOGD("type->entryCount=%x\n", type->entryCount));
                        const size_t N = type->entryCount;
                        for (size_t j=0; j<N; j++) {
                            if (typeBags[j] && typeBags[j] != (bag_set*)0xFFFFFFFF)
                                free(typeBags[j]);
                        }
                        free(typeBags);
                    }
                }
            }
            free(bags);
            bags = NULL;
        }
    }

    ResTable* const                 owner;
    String16 const                  name;
    uint32_t const                  id;
    Vector<Package*>                packages;

    // This is for finding typeStrings and other common package stuff.
    Package*                        basePackage;

    // For quick access.
    size_t                          typeCount;

    // Computed attribute bags, first indexed by the type and second
    // by the entry in that type.
    bag_set***                      bags;
};

struct ResTable::bag_set
{
    size_t numAttrs;    // number in array
    size_t availAttrs;  // total space in array
    uint32_t typeSpecFlags;
    // Followed by 'numAttr' bag_entry structures.
};

ResTable::ResTable(const void* data, size_t size, void* cookie, bool copyData)
    : mError(NO_INIT)
{
    memset(&mParams, 0, sizeof(mParams));
    memset(mPackageMap, 0, sizeof(mPackageMap));
    add(data, size, cookie, copyData);
    if(mError != NO_ERROR)
        LOGE("Error parsing resource table");
    //LOGI("Creating ResTable %p\n", this);
}


void ResTable::print_value(const Package* pkg, const Res_value& value,
        std::string * const utf8_value) const
{
    if (value.dataType == Res_value::TYPE_STRING)
    {
        size_t len;
        const char* str8 = pkg->header->values.string8At(value.data, &len);
        if (str8 != NULL)
        {
            if (utf8_value)
            {
                LINELOG();
                *utf8_value = std::string(str8);
                LOGD("(string8t) \"%s\"\n", utf8_value->c_str());
            }
//            LOGD("(string8) \"%s\"\n", normalizeForOutput(str8).string());
        }
        else
        {
            const char16_t* str16 = pkg->header->values.stringAt(value.data,
                    &len);
            if (str16 != NULL)
            {
                if (utf8_value)
                {
                    LINELOG();
                    *utf8_value = std::string(String8(str16, len).string());
                    LOGD("(string16t) \"%s\"\n", utf8_value->c_str());
                }
//                LOGD("商 %ld", String8(str16, len).find("商", 0));
//                LOGD("(string16) \"%s\"\n",
//                    normalizeForOutput(String8(str16, len).string()).string());
            }
            else
            {
                LOGD("(string) null\n");
            }
        }
    }
    else
    {
        LOGD("(unknown type) t=0x%02x d=0x%08x (s=0x%04x r=0x%02x)\n",
                (int)value.dataType, (int)value.data, (int)value.size, (int)value.res0);
    }
}

static status_t validate_chunk(const ResChunk_header* chunk,
                               size_t minSize,
                               const uint8_t* dataEnd,
                               const char* name)
{
    const uint16_t headerSize = dtohs(chunk->headerSize);
    const uint32_t size = dtohl(chunk->size);

    if (headerSize >= minSize) {
        if (headerSize <= size) {
            if (((headerSize|size)&0x3) == 0) {
                if ((ssize_t)size <= (dataEnd-((const uint8_t*)chunk))) {
                    return NO_ERROR;
                }
                LOGW("%s data size %p extends beyond resource end %p.",
                     name, (void*)size,
                     (void*)(dataEnd-((const uint8_t*)chunk)));
                return BAD_TYPE;
            }
            LOGW("%s size 0x%x or headerSize 0x%x is not on an integer boundary.",
                 name, (int)size, (int)headerSize);
            return BAD_TYPE;
        }
        LOGW("%s size %p is smaller than header size %p.",
             name, (void*)size, (void*)(int)headerSize);
        return BAD_TYPE;
    }
    LOGW("%s header size %p is too small.",
         name, (void*)(int)headerSize);
    return BAD_TYPE;
}

ResTable::~ResTable()
{
    //LOGI("Destroying ResTable in %p\n", this);
    uninit();
}

void ResTable::uninit()
{
    mError = NO_INIT;
    size_t N = mPackageGroups.size();
    for (size_t i=0; i<N; i++) {
        PackageGroup* g = mPackageGroups[i];
        delete g;
    }
    N = mHeaders.size();
    for (size_t i=0; i<N; i++) {
        Header* header = mHeaders[i];
        if (header->owner == this) {
            if (header->ownedData) {
                free(header->ownedData);
            }
            delete header;
        }
    }

    mPackageGroups.clear();
    mHeaders.clear();
}

// range checked; guaranteed to NUL-terminate within the stated number of available slots
// NOTE: if this truncates the dst string due to running out of space, no attempt is
// made to avoid splitting surrogate pairs.
static void strcpy16_dtoh(uint16_t* dst, const uint16_t* src, size_t avail)
{
    uint16_t* last = dst + avail - 1;
    while (*src && (dst < last)) {
        char16_t s = dtohs(*src);
        *dst++ = s;
        src++;
    }
    *dst = 0;
}

ssize_t ResTable::getEntry(
    const Package* package, int typeIndex, int entryIndex,
    const ResTable_config* config,
    const ResTable_type** outType, const ResTable_entry** outEntry,
    const Type** outTypeClass) const
{
    LOGD("Getting entry from package %p 0x%08x", package, entryIndex);
    const ResTable_package* const pkg = package->package;

    const Type* allTypes = package->getType(typeIndex);
    LOGD("allTypes=%p\n", allTypes);
    if (allTypes == NULL) {
        LOGD("Skipping entry type index 0x%02x because type is NULL!\n", typeIndex);
        return 0;
    }

    if ((size_t)entryIndex >= allTypes->entryCount) {
        LOGW("getEntry failing because entryIndex %d is beyond type entryCount %d",
            entryIndex, (int)allTypes->entryCount);
        return BAD_TYPE;
    }

    const ResTable_type* type = NULL;
    uint32_t offset = ResTable_type::NO_ENTRY;
    ResTable_config bestConfig;
    memset(&bestConfig, 0, sizeof(bestConfig)); // make the compiler shut up

    const size_t NT = allTypes->configs.size();
    for (size_t i=0; i<NT; i++) {
        const ResTable_type* const thisType = allTypes->configs[i];
        if (thisType == NULL) continue;

        ResTable_config thisConfig;
        thisConfig.copyFromDtoH(thisType->config);

        TABLE_GETENTRY(LOGI("Match entry 0x%x in type 0x%x (sz 0x%x): imsi:%d/%d=%d/%d "
                            "lang:%c%c=%c%c cnt:%c%c=%c%c orien:%d=%d touch:%d=%d "
                            "density:%d=%d key:%d=%d inp:%d=%d nav:%d=%d w:%d=%d h:%d=%d "
                            "swdp:%d=%d wdp:%d=%d hdp:%d=%d\n",
                           entryIndex, typeIndex+1, dtohl(thisType->config.size),
                           thisConfig.mcc, thisConfig.mnc,
                           config ? config->mcc : 0, config ? config->mnc : 0,
                           thisConfig.language[0] ? thisConfig.language[0] : '-',
                           thisConfig.language[1] ? thisConfig.language[1] : '-',
                           config && config->language[0] ? config->language[0] : '-',
                           config && config->language[1] ? config->language[1] : '-',
                           thisConfig.country[0] ? thisConfig.country[0] : '-',
                           thisConfig.country[1] ? thisConfig.country[1] : '-',
                           config && config->country[0] ? config->country[0] : '-',
                           config && config->country[1] ? config->country[1] : '-',
                           thisConfig.orientation,
                           config ? config->orientation : 0,
                           thisConfig.touchscreen,
                           config ? config->touchscreen : 0,
                           thisConfig.density,
                           config ? config->density : 0,
                           thisConfig.keyboard,
                           config ? config->keyboard : 0,
                           thisConfig.inputFlags,
                           config ? config->inputFlags : 0,
                           thisConfig.navigation,
                           config ? config->navigation : 0,
                           thisConfig.screenWidth,
                           config ? config->screenWidth : 0,
                           thisConfig.screenHeight,
                           config ? config->screenHeight : 0,
                           thisConfig.smallestScreenWidthDp,
                           config ? config->smallestScreenWidthDp : 0,
                           thisConfig.screenWidthDp,
                           config ? config->screenWidthDp : 0,
                           thisConfig.screenHeightDp,
                           config ? config->screenHeightDp : 0));

        // Check to make sure this one is valid for the current parameters.
        if (config && !thisConfig.match(*config)) {
            TABLE_GETENTRY(LOGI("Does not match config!\n"));
            continue;
        }

        // Check if there is the desired entry in this type.

        const uint8_t* const end = ((const uint8_t*)thisType)
            + dtohl(thisType->header.size);
        const uint32_t* const eindex = (const uint32_t*)
            (((const uint8_t*)thisType) + dtohs(thisType->header.headerSize));

        uint32_t thisOffset = dtohl(eindex[entryIndex]);
        if (thisOffset == ResTable_type::NO_ENTRY) {
            TABLE_GETENTRY(LOGI("Skipping because it is not defined!\n"));
            continue;
        }

        if (type != NULL) {
            // Check if this one is less specific than the last found.  If so,
            // we will skip it.  We check starting with things we most care
            // about to those we least care about.
            if (!thisConfig.isBetterThan(bestConfig, config)) {
                TABLE_GETENTRY(LOGI("This config is worse than last!\n"));
                continue;
            }
        }

        type = thisType;
        offset = thisOffset;
        bestConfig = thisConfig;
        TABLE_GETENTRY(LOGI("Best entry so far -- using it!\n"));
        if (!config) break;
    }

    if (type == NULL) {
        TABLE_GETENTRY(LOGI("No value found for requested entry!\n"));
        return BAD_INDEX;
    }

    offset += dtohl(type->entriesStart);

    if (offset > (dtohl(type->header.size)-sizeof(ResTable_entry))) {
        LOGW("ResTable_entry at 0x%x is beyond type chunk data 0x%x",
             offset, dtohl(type->header.size));
        return BAD_TYPE;
    }
    if ((offset&0x3) != 0) {
        LOGW("ResTable_entry at 0x%x is not on an integer boundary",
             offset);
        return BAD_TYPE;
    }

    const ResTable_entry* const entry = (const ResTable_entry*)
        (((const uint8_t*)type) + offset);
    if (dtohs(entry->size) < sizeof(*entry)) {
        LOGW("ResTable_entry size 0x%x is too small", dtohs(entry->size));
        return BAD_TYPE;
    }

    *outType = type;
    *outEntry = entry;
    if (outTypeClass != NULL) {
        *outTypeClass = allTypes;
    }
    return offset + dtohs(entry->size);
}

status_t ResTable::parsePackage(const ResTable_package* const pkg,
                                const Header* const header, uint32_t idmap_id)
{
    const uint8_t* base = (const uint8_t*)pkg;
    status_t err = validate_chunk(&pkg->header, sizeof(*pkg),
                                  header->dataEnd, "ResTable_package");
    if (err != NO_ERROR) {
        return (mError=err);
    }

    const size_t pkgSize = dtohl(pkg->header.size);

    if (dtohl(pkg->typeStrings) >= pkgSize) {
        LOGW("ResTable_package type strings at %p are past chunk size %p.",
             (void*)dtohl(pkg->typeStrings), (void*)pkgSize);
        return (mError=BAD_TYPE);
    }
    if ((dtohl(pkg->typeStrings)&0x3) != 0) {
        LOGW("ResTable_package type strings at %p is not on an integer boundary.",
             (void*)dtohl(pkg->typeStrings));
        return (mError=BAD_TYPE);
    }
    if (dtohl(pkg->keyStrings) >= pkgSize) {
        LOGW("ResTable_package key strings at %p are past chunk size %p.",
             (void*)dtohl(pkg->keyStrings), (void*)pkgSize);
        return (mError=BAD_TYPE);
    }
    if ((dtohl(pkg->keyStrings)&0x3) != 0) {
        LOGW("ResTable_package key strings at %p is not on an integer boundary.",
             (void*)dtohl(pkg->keyStrings));
        return (mError=BAD_TYPE);
    }

    Package* package = NULL;
    PackageGroup* group = NULL;
    uint32_t id = idmap_id != 0 ? idmap_id : dtohl(pkg->id);
    // If at this point id == 0, pkg is an overlay package without a
    // corresponding idmap. During regular usage, overlay packages are
    // always loaded alongside their idmaps, but during idmap creation
    // the package is temporarily loaded by itself.
    if (id < 256) {

        package = new Package(this, header, pkg);
        if (package == NULL) {
            return (mError=NO_MEMORY);
        }

        size_t idx = mPackageMap[id];
        if (idx == 0) {
            idx = mPackageGroups.size()+1;

            char16_t tmpName[sizeof(pkg->name)/sizeof(char16_t)];
            strcpy16_dtoh(tmpName, pkg->name, sizeof(pkg->name)/sizeof(char16_t));
            group = new PackageGroup(this, String16(tmpName), id);
            if (group == NULL) {
                delete package;
                return (mError=NO_MEMORY);
            }

            err = package->typeStrings.setTo(base+dtohl(pkg->typeStrings),
                                           header->dataEnd-(base+dtohl(pkg->typeStrings)));
            if (err != NO_ERROR) {
                delete group;
                delete package;
                return (mError=err);
            }
            err = package->keyStrings.setTo(base+dtohl(pkg->keyStrings),
                                          header->dataEnd-(base+dtohl(pkg->keyStrings)));
            if (err != NO_ERROR) {
                delete group;
                delete package;
                return (mError=err);
            }

            //LOGD("Adding new package id %d at index %d\n", id, idx);
            err = mPackageGroups.add(group);
            if (err < NO_ERROR) {
                return (mError=err);
            }
            group->basePackage = package;

            mPackageMap[id] = (uint8_t)idx;
        } else {
            group = mPackageGroups.itemAt(idx-1);
            if (group == NULL) {
                return (mError=UNKNOWN_ERROR);
            }
        }
        err = group->packages.add(package);
        if (err < NO_ERROR) {
            return (mError=err);
        }
    } else {
        LOG_ALWAYS_FATAL("Package id out of range");
        return NO_ERROR;
    }


    // Iterate through all chunks.
    size_t curPackage = 0;

    const ResChunk_header* chunk =
        (const ResChunk_header*)(((const uint8_t*)pkg)
                                 + dtohs(pkg->header.headerSize));
    const uint8_t* endPos = ((const uint8_t*)pkg) + dtohs(pkg->header.size);
    while (((const uint8_t*)chunk) <= (endPos-sizeof(ResChunk_header)) &&
           ((const uint8_t*)chunk) <= (endPos-dtohl(chunk->size))) {
        TABLE_NOISY(LOGD("PackageChunk: type=0x%x, headerSize=0x%x, size=0x%x, pos=%p\n",
                         dtohs(chunk->type), dtohs(chunk->headerSize), dtohl(chunk->size),
                         (void*)(((const uint8_t*)chunk) - ((const uint8_t*)header->header))));
        const size_t csize = dtohl(chunk->size);
        const uint16_t ctype = dtohs(chunk->type);
        if (ctype == RES_TABLE_TYPE_SPEC_TYPE) {
            const ResTable_typeSpec* typeSpec = (const ResTable_typeSpec*)(chunk);
            err = validate_chunk(&typeSpec->header, sizeof(*typeSpec),
                                 endPos, "ResTable_typeSpec");
            if (err != NO_ERROR) {
                return (mError=err);
            }

            const size_t typeSpecSize = dtohl(typeSpec->header.size);

            LOAD_TABLE_NOISY(LOGD("TypeSpec off %p: type=0x%x, headerSize=0x%x",
                                    (void*)(base-(const uint8_t*)chunk),
                                    dtohs(typeSpec->header.type),
                                    dtohs(typeSpec->header.headerSize),
                                    ));
            // look for block overrun or int overflow when multiplying by 4
            if ((dtohl(typeSpec->entryCount) > (INT32_MAX/sizeof(uint32_t))
                    || dtohs(typeSpec->header.headerSize)+(sizeof(uint32_t)*dtohl(typeSpec->entryCount))
                    > typeSpecSize)) {
                LOGW("ResTable_typeSpec entry index to %p extends beyond chunk end %p.",
                     (void*)(dtohs(typeSpec->header.headerSize)
                             +(sizeof(uint32_t)*dtohl(typeSpec->entryCount))),
                     (void*)typeSpecSize);
                return (mError=BAD_TYPE);
            }

            if (typeSpec->id == 0) {
                LOGW("ResTable_type has an id of 0.");
                return (mError=BAD_TYPE);
            }

            while (package->types.size() < typeSpec->id) {
                package->types.add(NULL);
            }
            Type* t = package->types[typeSpec->id-1];
            if (t == NULL) {
                t = new Type(header, package, dtohl(typeSpec->entryCount));
                package->types.editItemAt(typeSpec->id-1) = t;
            } else if (dtohl(typeSpec->entryCount) != t->entryCount) {
                LOGW("ResTable_typeSpec entry count inconsistent: given %d, previously %d",
                    (int)dtohl(typeSpec->entryCount), (int)t->entryCount);
                return (mError=BAD_TYPE);
            }
            t->typeSpecFlags = (const uint32_t*)(
                    ((const uint8_t*)typeSpec) + dtohs(typeSpec->header.headerSize));
            t->typeSpec = typeSpec;

        } else if (ctype == RES_TABLE_TYPE_TYPE) {
            const ResTable_type* type = (const ResTable_type*)(chunk);
            err = validate_chunk(&type->header, sizeof(*type)-sizeof(ResTable_config)+4,
                                 endPos, "ResTable_type");
            if (err != NO_ERROR) {
                return (mError=err);
            }

            const size_t typeSize = dtohl(type->header.size);

            LOAD_TABLE_NOISY(LOGD("Type off %p: type=0x%x, headerSize=0x%x, size=%p\n",
                                    (void*)(base-(const uint8_t*)chunk),
                                    dtohs(type->header.type),
                                    dtohs(type->header.headerSize),
                                    (void*)typeSize));
            if (dtohs(type->header.headerSize)+(sizeof(uint32_t)*dtohl(type->entryCount))
                > typeSize) {
                LOGW("ResTable_type entry index to %p extends beyond chunk end %p.",
                     (void*)(dtohs(type->header.headerSize)
                             +(sizeof(uint32_t)*dtohl(type->entryCount))),
                     (void*)typeSize);
                return (mError=BAD_TYPE);
            }
            if (dtohl(type->entryCount) != 0
                && dtohl(type->entriesStart) > (typeSize-sizeof(ResTable_entry))) {
                LOGW("ResTable_type entriesStart at %p extends beyond chunk end %p.",
                     (void*)dtohl(type->entriesStart), (void*)typeSize);
                return (mError=BAD_TYPE);
            }
            if (type->id == 0) {
                LOGW("ResTable_type has an id of 0.");
                return (mError=BAD_TYPE);
            }

            while (package->types.size() < type->id) {
                package->types.add(NULL);
            }
            Type* t = package->types[type->id-1];
            if (t == NULL) {
                t = new Type(header, package, dtohl(type->entryCount));
                package->types.editItemAt(type->id-1) = t;
            } else if (dtohl(type->entryCount) != t->entryCount) {
                LOGW("ResTable_type entry count inconsistent: given %d, previously %d",
                    (int)dtohl(type->entryCount), (int)t->entryCount);
                return (mError=BAD_TYPE);
            }

            TABLE_GETENTRY(
                ResTable_config thisConfig;
                thisConfig.copyFromDtoH(type->config);
                LOGI("Adding config to type %d: imsi:%d/%d lang:%c%c cnt:%c%c "
                     "orien:%d touch:%d density:%d key:%d inp:%d nav:%d w:%d h:%d "
                     "swdp:%d wdp:%d hdp:%d\n",
                      type->id,
                      thisConfig.mcc, thisConfig.mnc,
                      thisConfig.language[0] ? thisConfig.language[0] : '-',
                      thisConfig.language[1] ? thisConfig.language[1] : '-',
                      thisConfig.country[0] ? thisConfig.country[0] : '-',
                      thisConfig.country[1] ? thisConfig.country[1] : '-',
                      thisConfig.orientation,
                      thisConfig.touchscreen,
                      thisConfig.density,
                      thisConfig.keyboard,
                      thisConfig.inputFlags,
                      thisConfig.navigation,
                      thisConfig.screenWidth,
                      thisConfig.screenHeight,
                      thisConfig.smallestScreenWidthDp,
                      thisConfig.screenWidthDp,
                      thisConfig.screenHeightDp));
            t->configs.add(type);
        } else {
            status_t err = validate_chunk(chunk, sizeof(ResChunk_header),
                                          endPos, "ResTable_package:unknown");
            if (err != NO_ERROR) {
                return (mError=err);
            }
        }
        chunk = (const ResChunk_header*)
            (((const uint8_t*)chunk) + csize);
    }

    if (group->typeCount == 0) {
        group->typeCount = package->types.size();
    }

    return NO_ERROR;
}

status_t ResTable::add(const void* data, size_t size, void* cookie,
                       bool copyData)
{
    if (!data) return NO_ERROR;
    Header* header = new Header(this);
    header->index = mHeaders.size();
    header->cookie = cookie;
//    if (idmap != NULL) {
//        const size_t idmap_size = idmap->getLength();
//        const void* idmap_data = const_cast<Asset*>(idmap)->getBuffer(true);
//        header->resourceIDMap = (uint32_t*)malloc(idmap_size);
//        if (header->resourceIDMap == NULL) {
//            delete header;
//            return (mError = NO_MEMORY);
//        }
//        memcpy((void*)header->resourceIDMap, idmap_data, idmap_size);
//        header->resourceIDMapSize = idmap_size;
//    }
    mHeaders.add(header);

    const bool notDeviceEndian = htods(0xf0) != 0xf0;

    LOAD_TABLE_NOISY(
        LOGD("Adding resources to ResTable: data=%p, size=0x%x, cookie=%p, copy=%d\n",
                data, size, cookie, copyData));

    if (copyData || notDeviceEndian) {
        header->ownedData = malloc(size);
        if (header->ownedData == NULL) {
            return (mError=NO_MEMORY);
        }
        memcpy(header->ownedData, data, size);
        data = header->ownedData;
    }

    header->header = (const ResTable_header*)data;
    header->size = dtohl(header->header->header.size);
    //LOGI("Got size 0x%x, again size 0x%x, raw size 0x%x\n", header->size,
    //     dtohl(header->header->header.size), header->header->header.size);
    LOAD_TABLE_NOISY(LOGD("Loading ResTable @%p:\n", header->header));
//    LOAD_TABLE_NOISY(printHexData(2, header->header, header->size < 256 ? header->size : 256,
//                                  16, 16, 0, false, printToLogFunc));
    if (dtohs(header->header->header.headerSize) > header->size
            || header->size > size) {
        LOGW("Bad resource table: header size 0x%x or total size 0x%x is larger than data size 0x%x\n",
             (int)dtohs(header->header->header.headerSize),
             (int)header->size, (int)size);
        return (mError=BAD_TYPE);
    }
    if (((dtohs(header->header->header.headerSize)|header->size)&0x3) != 0) {
        LOGW("Bad resource table: header size 0x%x or total size 0x%x is not on an integer boundary\n",
             (int)dtohs(header->header->header.headerSize),
             (int)header->size);
        return (mError=BAD_TYPE);
    }
    header->dataEnd = ((const uint8_t*)header->header) + header->size;

    // Iterate through all chunks.
    size_t curPackage = 0;

    const ResChunk_header* chunk =
        (const ResChunk_header*)(((const uint8_t*)header->header)
                                 + dtohs(header->header->header.headerSize));
    while (((const uint8_t*)chunk) <= (header->dataEnd-sizeof(ResChunk_header)) &&
           ((const uint8_t*)chunk) <= (header->dataEnd-dtohl(chunk->size))) {
        status_t err = validate_chunk(chunk, sizeof(ResChunk_header), header->dataEnd, "ResTable");
        if (err != NO_ERROR) {
            return (mError=err);
        }
        TABLE_NOISY(LOGD("Chunk: type=0x%x, headerSize=0x%x, size=0x%x, pos=%p\n",
                     dtohs(chunk->type), dtohs(chunk->headerSize), dtohl(chunk->size),
                     (void*)(((const uint8_t*)chunk) - ((const uint8_t*)header->header))));
        const size_t csize = dtohl(chunk->size);
        const uint16_t ctype = dtohs(chunk->type);
        if (ctype == RES_STRING_POOL_TYPE) {
            if (header->values.getError() != NO_ERROR) {
                // Only use the first string chunk; ignore any others that
                // may appear.
                status_t err = header->values.setTo(chunk, csize);
                if (err != NO_ERROR) {
                    return (mError=err);
                }
            } else {
                LOGW("Multiple string chunks found in resource table.");
            }
        } else if (ctype == RES_TABLE_PACKAGE_TYPE) {
            if (curPackage >= dtohl(header->header->packageCount)) {
                LOGW("More package chunks were found than the %d declared in the header.",
                     dtohl(header->header->packageCount));
                return (mError=BAD_TYPE);
            }
            uint32_t idmap_id = 0;
//            if (idmap != NULL) {
//                uint32_t tmp;
//                if (getIdmapPackageId(header->resourceIDMap,
//                                      header->resourceIDMapSize,
//                                      &tmp) == NO_ERROR) {
//                    idmap_id = tmp;
//                }
//            }
            if (parsePackage((ResTable_package*)chunk, header, idmap_id) != NO_ERROR) {
                return mError;
            }
            curPackage++;
        } else {
            LOGW("Unknown chunk type %p in table at %p.\n",
                 (void*)(int)(ctype),
                 (void*)(((const uint8_t*)chunk) - ((const uint8_t*)header->header)));
        }
        chunk = (const ResChunk_header*)
            (((const uint8_t*)chunk) + csize);
    }

    if (curPackage < dtohl(header->header->packageCount)) {
        LOGW("Fewer package chunks (%d) were found than the %d declared in the header.",
             (int)curPackage, dtohl(header->header->packageCount));
        return (mError=BAD_TYPE);
    }
    mError = header->values.getError();
    if (mError != NO_ERROR) {
        LOGW("No string values found in resource table!");
    }

    TABLE_NOISY(LOGD("Returning from add with mError=%d\n", mError));
    return mError;
}


inline ssize_t ResTable::getResourcePackageIndex(uint32_t resID) const
{
    return ((ssize_t)mPackageMap[Res_GETPACKAGE(resID)+1])-1;
}

inline void Res_value::copyFrom_dtoh(const Res_value& src)
{
    size = dtohs(src.size);
    res0 = src.res0;
    dataType = src.dataType;
    data = dtohl(src.data);
}

bool ResTable::getResourceName(uint32_t resID, resource_name* outName) const
{
    if (mError != NO_ERROR) {
        LOGW("mError %d", (int32_t)mError);
        return false;
    }

    const ssize_t p = getResourcePackageIndex(resID);
    const int t = Res_GETTYPE(resID);
    const int e = Res_GETENTRY(resID);

    if (p < 0) {
        if (Res_GETPACKAGE(resID)+1 == 0) {
            LOGW("No package identifier when getting name for resource number 0x%08x", resID);
        } else {
            LOGW("No known package when getting name for resource number 0x%08x", resID);
        }
        return false;
    }
    if (t < 0) {
        LOGW("No type identifier when getting name for resource number 0x%08x", resID);
        return false;
    }

    const PackageGroup* const grp = mPackageGroups[p];
    if (grp == NULL) {
        LOGW("Bad identifier when getting name for resource number 0x%08x", resID);
        return false;
    }
    if (grp->packages.size() > 0) {
        const Package* const package = grp->packages[0];

        const ResTable_type* type;
        const ResTable_entry* entry;
        ssize_t offset = getEntry(package, t, e, NULL, &type, &entry, NULL);
        if (offset <= 0) {
            LOGW("offset %ld <= 0", offset);
            return false;
        }

        outName->package = grp->name.string();
        outName->packageLen = grp->name.size();
        outName->type = grp->basePackage->typeStrings.stringAt(t, &outName->typeLen);
        outName->name = grp->basePackage->keyStrings.stringAt(
            dtohl(entry->key.index), &outName->nameLen);

        // If we have a bad index for some reason, we should abort.
        if (outName->type == NULL || outName->name == NULL) {
            LOGW("bad index");
            return false;
        }

        return true;
    }

    LOGW("bad index false 0x%08x", resID);
    return false;
}

void ResTable::print(bool inclValues) const
{
    size_t pgCount = mPackageGroups.size();
    LOGD("Package Groups (%d)\n", (int)pgCount);
    for (size_t pgIndex=0; pgIndex<pgCount; pgIndex++) {
        const PackageGroup* pg = mPackageGroups[pgIndex];
        LOGD("Package Group %d id=%d packageCount=%d name=%s\n",
                (int)pgIndex, pg->id, (int)pg->packages.size(),
                String8(pg->name).string());

        size_t pkgCount = pg->packages.size();
        for (size_t pkgIndex=0; pkgIndex<pkgCount; pkgIndex++) {
            const Package* pkg = pg->packages[pkgIndex];
            size_t typeCount = pkg->types.size();
            LOGD("  Package %d id=%d name=%s typeCount=%d\n", (int)pkgIndex,
                    pkg->package->id, String8(String16(pkg->package->name)).string(),
                    (int)typeCount);
            for (size_t typeIndex=0; typeIndex<typeCount; typeIndex++) {
                const Type* typeConfigs = pkg->getType(typeIndex);
                if (typeConfigs == NULL) {
                    LOGD("    type %d NULL\n", (int)typeIndex);
                    continue;
                }
                const size_t NTC = typeConfigs->configs.size();
                LOGD("    type %d configCount=%d entryCount=%d\n",
                       (int)typeIndex, (int)NTC, (int)typeConfigs->entryCount);
                if (typeConfigs->typeSpecFlags != NULL) {
                    for (size_t entryIndex=0; entryIndex<typeConfigs->entryCount; entryIndex++) {
                        uint32_t resID = (0xff000000 & ((pkg->package->id)<<24))
                                    | (0x00ff0000 & ((typeIndex+1)<<16))
                                    | (0x0000ffff & (entryIndex));
                        resource_name resName;
                        LOGD("resID 0x%08x", resID);
                        if (this->getResourceName(resID, &resName)) {
                            LOGD("      spec resource 0x%08x %s:%s/%s: flags=0x%08x\n",
                                resID,
                                CHAR16_TO_CSTR(resName.package, resName.packageLen),
                                CHAR16_TO_CSTR(resName.type, resName.typeLen),
                                CHAR16_TO_CSTR(resName.name, resName.nameLen),
                                dtohl(typeConfigs->typeSpecFlags[entryIndex]));
                        } else {
                            LOGD("      INVALID TYPE CONFIG FOR RESOURCE 0x%08x\n", resID);
                        }
                    }
                }
                for (size_t configIndex=0; configIndex<NTC; configIndex++) {
                    const ResTable_type* type = typeConfigs->configs[configIndex];
                    if ((((uint64_t)type)&0x3) != 0) {
                        LOGD("      NON-INTEGER ResTable_type ADDRESS: %p\n", type);
                        continue;
                    }
                    char density[16];
                    uint16_t dval = dtohs(type->config.density);
                    if (dval == ResTable_config::DENSITY_DEFAULT) {
                        strcpy(density, "def");
                    } else if (dval == ResTable_config::DENSITY_NONE) {
                        strcpy(density, "no");
                    } else {
                        LOGD(density, "%d", (int)dval);
                    }
                    LOGD("      config %d", (int)configIndex);
                    if (type->config.mcc != 0) {
                        LOGD(" mcc=%d", dtohs(type->config.mcc));
                    }
                    if (type->config.mnc != 0) {
                        LOGD(" mnc=%d", dtohs(type->config.mnc));
                    }
                    if (type->config.locale != 0) {
                        LOGD(" lang=%c%c cnt=%c%c",
                               type->config.language[0] ? type->config.language[0] : '-',
                               type->config.language[1] ? type->config.language[1] : '-',
                               type->config.country[0] ? type->config.country[0] : '-',
                               type->config.country[1] ? type->config.country[1] : '-');
                    }
                    if (type->config.screenLayout != 0) {
                        LOGD(" sz=%d",
                                type->config.screenLayout&ResTable_config::MASK_SCREENSIZE);
                        switch (type->config.screenLayout&ResTable_config::MASK_SCREENSIZE) {
                            case ResTable_config::SCREENSIZE_SMALL:
                                LOGD(" (small)");
                                break;
                            case ResTable_config::SCREENSIZE_NORMAL:
                                LOGD(" (normal)");
                                break;
                            case ResTable_config::SCREENSIZE_LARGE:
                                LOGD(" (large)");
                                break;
                            case ResTable_config::SCREENSIZE_XLARGE:
                                LOGD(" (xlarge)");
                                break;
                        }
                        LOGD(" lng=%d",
                                type->config.screenLayout&ResTable_config::MASK_SCREENLONG);
                        switch (type->config.screenLayout&ResTable_config::MASK_SCREENLONG) {
                            case ResTable_config::SCREENLONG_NO:
                                LOGD(" (notlong)");
                                break;
                            case ResTable_config::SCREENLONG_YES:
                                LOGD(" (long)");
                                break;
                        }
                    }
                    if (type->config.orientation != 0) {
                        LOGD(" orient=%d", type->config.orientation);
                        switch (type->config.orientation) {
                            case ResTable_config::ORIENTATION_PORT:
                                LOGD(" (port)");
                                break;
                            case ResTable_config::ORIENTATION_LAND:
                                LOGD(" (land)");
                                break;
                            case ResTable_config::ORIENTATION_SQUARE:
                                LOGD(" (square)");
                                break;
                        }
                    }
                    if (type->config.uiMode != 0) {
                        LOGD(" type=%d",
                                type->config.uiMode&ResTable_config::MASK_UI_MODE_TYPE);
                        switch (type->config.uiMode&ResTable_config::MASK_UI_MODE_TYPE) {
                            case ResTable_config::UI_MODE_TYPE_NORMAL:
                                LOGD(" (normal)");
                                break;
                            case ResTable_config::UI_MODE_TYPE_CAR:
                                LOGD(" (car)");
                                break;
                        }
                        LOGD(" night=%d",
                                type->config.uiMode&ResTable_config::MASK_UI_MODE_NIGHT);
                        switch (type->config.uiMode&ResTable_config::MASK_UI_MODE_NIGHT) {
                            case ResTable_config::UI_MODE_NIGHT_NO:
                                LOGD(" (no)");
                                break;
                            case ResTable_config::UI_MODE_NIGHT_YES:
                                LOGD(" (yes)");
                                break;
                        }
                    }
                    if (dval != 0) {
                        LOGD(" density=%s", density);
                    }
                    if (type->config.touchscreen != 0) {
                        LOGD(" touch=%d", type->config.touchscreen);
                        switch (type->config.touchscreen) {
                            case ResTable_config::TOUCHSCREEN_NOTOUCH:
                                LOGD(" (notouch)");
                                break;
                            case ResTable_config::TOUCHSCREEN_STYLUS:
                                LOGD(" (stylus)");
                                break;
                            case ResTable_config::TOUCHSCREEN_FINGER:
                                LOGD(" (finger)");
                                break;
                        }
                    }
                    if (type->config.inputFlags != 0) {
                        LOGD(" keyhid=%d", type->config.inputFlags&ResTable_config::MASK_KEYSHIDDEN);
                        switch (type->config.inputFlags&ResTable_config::MASK_KEYSHIDDEN) {
                            case ResTable_config::KEYSHIDDEN_NO:
                                LOGD(" (no)");
                                break;
                            case ResTable_config::KEYSHIDDEN_YES:
                                LOGD(" (yes)");
                                break;
                            case ResTable_config::KEYSHIDDEN_SOFT:
                                LOGD(" (soft)");
                                break;
                        }
                        LOGD(" navhid=%d", type->config.inputFlags&ResTable_config::MASK_NAVHIDDEN);
                        switch (type->config.inputFlags&ResTable_config::MASK_NAVHIDDEN) {
                            case ResTable_config::NAVHIDDEN_NO:
                                LOGD(" (no)");
                                break;
                            case ResTable_config::NAVHIDDEN_YES:
                                LOGD(" (yes)");
                                break;
                        }
                    }
                    if (type->config.keyboard != 0) {
                        LOGD(" kbd=%d", type->config.keyboard);
                        switch (type->config.keyboard) {
                            case ResTable_config::KEYBOARD_NOKEYS:
                                LOGD(" (nokeys)");
                                break;
                            case ResTable_config::KEYBOARD_QWERTY:
                                LOGD(" (qwerty)");
                                break;
                            case ResTable_config::KEYBOARD_12KEY:
                                LOGD(" (12key)");
                                break;
                        }
                    }
                    if (type->config.navigation != 0) {
                        LOGD(" nav=%d", type->config.navigation);
                        switch (type->config.navigation) {
                            case ResTable_config::NAVIGATION_NONAV:
                                LOGD(" (nonav)");
                                break;
                            case ResTable_config::NAVIGATION_DPAD:
                                LOGD(" (dpad)");
                                break;
                            case ResTable_config::NAVIGATION_TRACKBALL:
                                LOGD(" (trackball)");
                                break;
                            case ResTable_config::NAVIGATION_WHEEL:
                                LOGD(" (wheel)");
                                break;
                        }
                    }
                    if (type->config.screenWidth != 0) {
                        LOGD(" w=%d", dtohs(type->config.screenWidth));
                    }
                    if (type->config.screenHeight != 0) {
                        LOGD(" h=%d", dtohs(type->config.screenHeight));
                    }
                    if (type->config.smallestScreenWidthDp != 0) {
                        LOGD(" swdp=%d", dtohs(type->config.smallestScreenWidthDp));
                    }
                    if (type->config.screenWidthDp != 0) {
                        LOGD(" wdp=%d", dtohs(type->config.screenWidthDp));
                    }
                    if (type->config.screenHeightDp != 0) {
                        LOGD(" hdp=%d", dtohs(type->config.screenHeightDp));
                    }
                    if (type->config.sdkVersion != 0) {
                        LOGD(" sdk=%d", dtohs(type->config.sdkVersion));
                    }
                    if (type->config.minorVersion != 0) {
                        LOGD(" mver=%d", dtohs(type->config.minorVersion));
                    }
                    LOGD("\n");
                    size_t entryCount = dtohl(type->entryCount);
                    uint32_t entriesStart = dtohl(type->entriesStart);
                    if ((entriesStart&0x3) != 0) {
                        LOGD("      NON-INTEGER ResTable_type entriesStart OFFSET: %p\n", (void*)entriesStart);
                        continue;
                    }
                    uint32_t typeSize = dtohl(type->header.size);
                    if ((typeSize&0x3) != 0) {
                        LOGD("      NON-INTEGER ResTable_type header.size: %p\n", (void*)typeSize);
                        continue;
                    }
                    for (size_t entryIndex=0; entryIndex<entryCount; entryIndex++) {

                        const uint8_t* const end = ((const uint8_t*)type)
                            + dtohl(type->header.size);
                        const uint32_t* const eindex = (const uint32_t*)
                            (((const uint8_t*)type) + dtohs(type->header.headerSize));

                        uint32_t thisOffset = dtohl(eindex[entryIndex]);
                        if (thisOffset == ResTable_type::NO_ENTRY) {
                            continue;
                        }

                        uint32_t resID = (0xff000000 & ((pkg->package->id)<<24))
                                    | (0x00ff0000 & ((typeIndex+1)<<16))
                                    | (0x0000ffff & (entryIndex));
                        resource_name resName;
                        if (this->getResourceName(resID, &resName)) {
                            LOGD("        resource 0x%08x %s:%s/%s: ", resID,
                                    CHAR16_TO_CSTR(resName.package, resName.packageLen),
                                    CHAR16_TO_CSTR(resName.type, resName.typeLen),
                                    CHAR16_TO_CSTR(resName.name, resName.nameLen));
                        } else {
                            LOGD("        INVALID RESOURCE 0x%08x: ", resID);
                        }
                        if ((thisOffset&0x3) != 0) {
                            LOGD("NON-INTEGER OFFSET: %p\n", (void*)thisOffset);
                            continue;
                        }
                        if ((thisOffset+sizeof(ResTable_entry)) > typeSize) {
                            LOGD("OFFSET OUT OF BOUNDS: %p+%p (size is %p)\n",
                                   (void*)entriesStart, (void*)thisOffset,
                                   (void*)typeSize);
                            continue;
                        }

                        const ResTable_entry* ent = (const ResTable_entry*)
                            (((const uint8_t*)type) + entriesStart + thisOffset);
                        if (((entriesStart + thisOffset)&0x3) != 0) {
                            LOGD("NON-INTEGER ResTable_entry OFFSET: %p\n",
                                 (void*)(entriesStart + thisOffset));
                            continue;
                        }

                        uint16_t esize = dtohs(ent->size);
                        if ((esize&0x3) != 0) {
                            LOGD("NON-INTEGER ResTable_entry SIZE: %p\n", (void*)esize);
                            continue;
                        }
                        if ((thisOffset+esize) > typeSize) {
                            LOGD("ResTable_entry OUT OF BOUNDS: %p+%p+%p (size is %p)\n",
                                   (void*)entriesStart, (void*)thisOffset,
                                   (void*)esize, (void*)typeSize);
                            continue;
                        }

                        const Res_value* valuePtr = NULL;
                        const ResTable_map_entry* bagPtr = NULL;
                        Res_value value;
                        if ((dtohs(ent->flags)&ResTable_entry::FLAG_COMPLEX) != 0) {
                            LOGD("<bag>");
                            bagPtr = (const ResTable_map_entry*)ent;
                        } else {
                            valuePtr = (const Res_value*)
                                (((const uint8_t*)ent) + esize);
                            value.copyFrom_dtoh(*valuePtr);
                            LOGD("t=0x%02x d=0x%08x (s=0x%04x r=0x%02x)",
                                   (int)value.dataType, (int)value.data,
                                   (int)value.size, (int)value.res0);
                        }

                        if ((dtohs(ent->flags)&ResTable_entry::FLAG_PUBLIC) != 0) {
                            LOGD(" (PUBLIC)");
                        }
                        LOGD("\n");

                        if (inclValues) {
                            if (valuePtr != NULL) {
                                LOGD("          ");
                                print_value(pkg, value);
                            } else if (bagPtr != NULL) {
                                const int N = dtohl(bagPtr->count);
                                const uint8_t* baseMapPtr = (const uint8_t*)ent;
                                size_t mapOffset = esize;
                                const ResTable_map* mapPtr = (ResTable_map*)(baseMapPtr+mapOffset);
                                LOGD("          Parent=0x%08x, Count=%d\n",
                                    dtohl(bagPtr->parent.ident), N);
                                for (int i=0; i<N && mapOffset < (typeSize-sizeof(ResTable_map)); i++) {
                                    LOGD("          #%i (Key=0x%08x): ",
                                        i, dtohl(mapPtr->name.ident));
                                    value.copyFrom_dtoh(mapPtr->value);
                                    print_value(pkg, value);
                                    const size_t size = dtohs(mapPtr->value.size);
                                    mapOffset += size + sizeof(*mapPtr)-sizeof(mapPtr->value);
                                    mapPtr = (ResTable_map*)(baseMapPtr+mapOffset);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

std::vector<std::string> * ResTable::getLabel(bool inclValues, uint32_t label_id) const
{
    std::vector<std::string> * label_vec = new std::vector<std::string>();
//    resource_name resName;
//    if (this->getResourceName(label_id, &resName)) {
//        LOGD("xxx      spec resource 0x%08x %s:%s/%s",
//            label_id,
//            CHAR16_TO_CSTR(resName.package, resName.packageLen),
//            CHAR16_TO_CSTR(resName.type, resName.typeLen),
//            CHAR16_TO_CSTR(resName.name, resName.nameLen));
//    } else {
//        LOGD("xxx      INVALID TYPE CONFIG FOR RESOURCE 0x%08x\n", label_id);
//    }
//

    size_t pgCount = mPackageGroups.size();
    LOGD("Package Groups (%d)\n", (int)pgCount);
    for (size_t pgIndex=0; pgIndex<pgCount; pgIndex++) {
        const PackageGroup* pg = mPackageGroups[pgIndex];
        LOGD("Package Group %d id=%d packageCount=%d name=%s\n",
                (int)pgIndex, pg->id, (int)pg->packages.size(),
                String8(pg->name).string());

        size_t pkgCount = pg->packages.size();
        for (size_t pkgIndex=0; pkgIndex<pkgCount; pkgIndex++) {
            const Package* pkg = pg->packages[pkgIndex];
            size_t typeCount = pkg->types.size();
            LOGD("  Package %d id=%d name=%s typeCount=%d\n", (int)pkgIndex,
                    pkg->package->id, String8(String16(pkg->package->name)).string(),
                    (int)typeCount);
            if ((0xff000000 & ((pkg->package->id)<<24)) == (0xff000000 & (label_id)))
            {
                LOGD("xxx got pkg");

                const Type* typeConfigs = pkg->getType(((0x00ff0000 & (label_id)) >> 16) - 1);
                const size_t NTC = typeConfigs->configs.size();

                for (size_t configIndex = 0; configIndex < NTC; configIndex++)
                {
                    const ResTable_type* type = typeConfigs->configs[configIndex];
                    if ((((uint64_t) type) & 0x3) == 0)
                    {
                        LOGD("NON-INTEGER ResTable_type ADDRESS: %p\n", type);
                        const uint32_t* const eindex = (const uint32_t*)
                            (((const uint8_t*)type) + dtohs(type->header.headerSize));

                        uint32_t thisOffset = dtohl(eindex[(0x0000ffff & (label_id))]);
                        uint32_t entriesStart = dtohl(type->entriesStart);

                        const ResTable_entry* ent = (const ResTable_entry*)
                            (((const uint8_t*)type) + entriesStart + thisOffset);

                        uint16_t esize = dtohs(ent->size);

                        const Res_value* valuePtr = NULL;
                        const ResTable_map_entry* bagPtr = NULL;
                        Res_value value;
                        if ((dtohs(ent->flags)&ResTable_entry::FLAG_COMPLEX) != 0) {
                            LOGD("<bag>");
                            bagPtr = (const ResTable_map_entry*)ent;
                        } else {
                            valuePtr = (const Res_value*)
                                (((const uint8_t*)ent) + esize);
                            value.copyFrom_dtoh(*valuePtr);
                            LOGD("t=0x%02x d=0x%08x (s=0x%04x r=0x%02x)",
                                   (int)value.dataType, (int)value.data,
                                   (int)value.size, (int)value.res0);
                        }

                        std::string utf8_label;
                        print_value(pkg, value, &utf8_label);
                        label_vec->push_back(utf8_label);
                    }
                }
            }
        }
    }
    return label_vec;
}
