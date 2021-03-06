#ifndef CARYLL_TABLES_GASP_H
#define CARYLL_TABLES_GASP_H

#include <support/util.h>
#include <font/caryll-sfnt.h>

typedef struct {
	uint16_t rangeMaxPPEM;
	bool dogray;
	bool gridfit;
	bool symmetric_smoothing;
	bool symmetric_gridfit;
} gasp_Record;
typedef struct {
	uint16_t version;
	uint16_t numRanges;
	gasp_Record *records;
} table_gasp;

table_gasp *table_new_gasp();
void table_delete_gasp(table_gasp *table);
table_gasp *table_read_gasp(caryll_Packet packet);
void table_dump_gasp(table_gasp *table, json_value *root, const caryll_Options *options);
table_gasp *table_parse_gasp(json_value *root, const caryll_Options *options);
caryll_buffer *table_build_gasp(table_gasp *table, const caryll_Options *options);
#endif
