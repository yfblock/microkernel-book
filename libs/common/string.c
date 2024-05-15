#include <libs/common/print.h>
#include <libs/common/string.h>

//比较内存的内容。
int memcmp(const void *p1, const void *p2, size_t len) {
    uint8_t *s1 = (uint8_t *) p1;
    uint8_t *s2 = (uint8_t *) p2;
    while (*s1 == *s2 && len > 0) {
        s1++;
        s2++;
        len--;
    }

    return (len > 0) ? *s1 - *s2 : 0;
}

//用指定的值填充内存区域的每个字节。
void *memset(void *dst, int ch, size_t len) {
    uint8_t *d = dst;
    while (len-- > 0) {
        *d = ch;
        d++;
    }
    return dst;
}

//复制一个内存区域。
void *memcpy(void *dst, const void *src, size_t len) {
    DEBUG_ASSERT(len < 256 * 1024 * 1024/*256MiB*/
                 && "too long memcpy (perhaps integer overflow?)");

    uint8_t *d = dst;
    const uint8_t *s = src;
    while (len-- > 0) {
        *d = *s;
        d++;
        s++;
    }
    return dst;
}

//复制一个内存区域。即使存在重叠，它也能正常工作。
void *memmove(void *dst, const void *src, size_t len) {
    DEBUG_ASSERT(len < 256 * 1024 * 1024/*256MiB*/
                 && "too long memmove (perhaps integer overflow?)");

    if ((uintptr_t) dst <= (uintptr_t) src) {
        memcpy(dst, src, len);
    } else {
        uint8_t *d = dst + len;
        const uint8_t *s = src + len;
        while (len-- > 0) {
            *d = *s;
            --d;
            --s;
        }
    }
    return dst;
}

//返回字符串的长度。
size_t strlen(const char *s) {
    size_t len = 0;
    while (*s != '\0') {
        len++;
        s++;
    }
    return len;
}

//比较字符串。如果相同，则返回 0。
int strcmp(const char *s1, const char *s2) {
    while (true) {
        if (*s1 != *s2) {
            return *s1 - *s2;
        }

        if (*s1 == '\0') {
            return 0;
        }

        s1++;
        s2++;
    }

    return 0;
}

//比较字符串直至指定的字符数。如果相同，则返回 0。
int strncmp(const char *s1, const char *s2, size_t len) {
    while (len > 0) {
        if (*s1 != *s2) {
            return *s1 - *s2;
        }

        if (*s1 == '\0') {
            //`*s1` 和 `*s2` 都等于 '\0'。
            break;
        }

        s1++;
        s2++;
        len--;
    }

    return 0;
}

//复制一个字符串。如果超过目标缓冲区大小，则仅复制适合缓冲区的内容。
char *strcpy_safe(char *dst, size_t dst_len, const char *src) {
    ASSERT(dst_len > 0);

    size_t i = 0;
    while (i < dst_len - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
    return dst;
}

//在字符串中搜索指定字符并返回其位置。
char *strchr(const char *str, int c) {
    char *s = (char *) str;
    while (*s != '\0') {
        if (*s == c) {
            return s;
        }

        s++;
    }

    return NULL;
}

//在字符串中搜索指定字符串并返回其位置。
char *strstr(const char *haystack, const char *needle) {
    char *s = (char *) haystack;
    size_t needle_len = strlen(needle);
    while (*s != '\0') {
        if (!strncmp(s, needle, needle_len)) {
            return s;
        }

        s++;
    }

    return NULL;
}

//将字符串转换为数字。仅支持十进制数字。
int atoi(const char *s) {
    int x = 0;
    while ('0' <= *s && *s <= '9') {
        x = (x * 10) + (*s - '0');
        s++;
    }

    return x;
}
