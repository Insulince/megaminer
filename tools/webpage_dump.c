/*
 * webpage_dump.c - PC tool: writes the web page from webpage.h to a file,
 * byte for byte, so tools/gzip_header.py can compress it for the chip.
 * Built and run by the Makefile (rule for webpage_gz.h):
 *
 *   gcc -I. -o webpage_dump.exe tools/webpage_dump.c
 *   webpage_dump.exe webpage.html
 *
 * The file is opened in binary mode, so Windows does not turn \n into \r\n.
 */
#include <stdio.h>

#include "webpage.h"

int main(int argc, char **argv)
{
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: webpage_dump OUT.html\n");
        return 2;
    }
    f = fopen(argv[1], "wb");
    if (!f || fwrite(WEB_PAGE, 1, sizeof WEB_PAGE - 1, f) != sizeof WEB_PAGE - 1 || fclose(f)) {
        fprintf(stderr, "webpage_dump: cannot write %s\n", argv[1]);
        return 1;
    }
    return 0;
}
