#ifndef NX_SEARCH_INDEX_INTERNAL_H
#define NX_SEARCH_INDEX_INTERNAL_H
#include "engine/nx_search_index.h"
typedef struct nx_prepared_posting { uint32_t row, tf; } nx_prepared_posting;
typedef struct nx_prepared_term { nx_slice text; uint32_t start, count; } nx_prepared_term;
typedef struct nx_prepared_trigram { uint32_t key, start, count; } nx_prepared_trigram;
typedef struct nx_prepared_field {
    bool text;
    uint8_t *text_bytes;
    nx_slice *normalized;
    uint32_t *lengths;
    uint32_t population, term_count, trigram_count;
    uint64_t total_length;
    nx_prepared_term *terms;
    nx_prepared_posting *postings;
    nx_prepared_trigram *trigrams;
    uint32_t *trigram_rows;
} nx_prepared_field;
struct nx_search_index {
    nx_table table;
    nx_prepared_field *fields;
    size_t memory_bytes;
};
#endif
