#ifndef MVLINK_H
#define MVLINK_H

extern int mavlink_port;

void* __MAVLINK_THREAD__(void* arg);

size_t numOfChars(const char s[]);

char* insertString(char s1[], const char s2[], size_t pos);

#endif