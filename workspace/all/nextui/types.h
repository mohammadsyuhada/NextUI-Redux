#ifndef TYPES_H
#define TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "defines.h"
#include "utils.h"

///////////////////////////////////////
// Array

typedef struct Array {
	int count;
	int capacity;
	void** items;
} Array;

Array* Array_new(void);
void Array_push(Array* self, void* item);
void Array_unshift(Array* self, void* item);
void* Array_pop(Array* self);
void Array_remove(Array* self, void* item);
void Array_reverse(Array* self);
void Array_free(Array* self);
void Array_yoink(Array* self, Array* other);

int StringArray_indexOf(Array* self, char* str);
void StringArray_free(Array* self);

///////////////////////////////////////
// Hash

typedef struct Hash {
	Array* keys;
	Array* values;
} Hash; // not really a hash

Hash* Hash_new(void);
void Hash_free(Hash* self);
void Hash_set(Hash* self, char* key, char* value);
char* Hash_get(Hash* self, char* key);

///////////////////////////////////////
// Entry

enum EntryType {
	ENTRY_DIR,
	ENTRY_PAK,
	ENTRY_ROM,
	ENTRY_DIP,
};

typedef struct Entry {
	char* path;
	char* name;
	char* unique;
	int type;
	int alpha; // index in parent Directory's alphas Array, which points to the index of an Entry in its entries Array :sweat_smile:
} Entry;

Entry* Entry_new(char* path, int type);
Entry* Entry_newNamed(char* path, int type, char* displayName);
void Entry_free(Entry* self);
int EntryArray_indexOf(Array* self, char* path);
int EntryArray_sortEntry(const void* a, const void* b);
void EntryArray_sort(Array* self);
void EntryArray_free(Array* self);

///////////////////////////////////////
// IntArray

#define INT_ARRAY_MAX 27
typedef struct IntArray {
	int count;
	int items[INT_ARRAY_MAX];
} IntArray;

IntArray* IntArray_new(void);
void IntArray_push(IntArray* self, int i);
void IntArray_free(IntArray* self);

///////////////////////////////////////
// Directory

typedef struct Directory {
	char* path;
	char* name;
	Array* entries;
	IntArray* alphas;
	// rendering
	int selected;
	int start;
	int end;
} Directory;

void Directory_free(Directory* self);
void DirectoryArray_pop(Array* self);
void DirectoryArray_free(Array* self);

#endif // TYPES_H
