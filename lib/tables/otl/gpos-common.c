#include "gpos-common.h"

void otl_delete_mark_array(otl_MarkArray *array) {
	if (array) {
		if (array->records) free(array->records);
		free(array);
	};
}
otl_Anchor otl_anchor_absent() {
	otl_Anchor anchor = {.present = false, .x = 0, .y = 0};
	return anchor;
}
otl_Anchor otl_read_anchor(font_file_pointer data, uint32_t tableLength, uint32_t offset) {
	otl_Anchor anchor = {.present = false, .x = 0, .y = 0};
	checkLength(offset + 6);
	anchor.present = true;
	anchor.x = read_16s(data + offset + 2);
	anchor.y = read_16s(data + offset + 4);
	return anchor;
FAIL:
	anchor.present = false;
	anchor.x = 0;
	anchor.y = 0;
	return anchor;
}
json_value *otl_dump_anchor(otl_Anchor a) {
	if (a.present) {
		json_value *v = json_object_new(2);
		json_object_push(v, "x", json_new_position(a.x));
		json_object_push(v, "y", json_new_position(a.y));
		return v;
	} else {
		return json_null_new();
	}
}
otl_Anchor otl_parse_anchor(json_value *v) {
	otl_Anchor anchor = {.present = false, .x = 0, .y = 0};
	if (!v || v->type != json_object) return anchor;
	anchor.present = true;
	anchor.x = json_obj_getnum_fallback(v, "x", 0);
	anchor.y = json_obj_getnum_fallback(v, "y", 0);
	return anchor;
}

otl_MarkArray *otl_read_mark_array(font_file_pointer data, uint32_t tableLength, uint32_t offset) {
	otl_MarkArray *array = NULL;
	NEW(array);
	checkLength(offset + 2);
	array->markCount = read_16u(data + offset);
	checkLength(offset + 2 + 4 * array->markCount);
	NEW_N(array->records, array->markCount);
	for (glyphid_t j = 0; j < array->markCount; j++) {
		array->records[j].markClass = read_16u(data + offset + 2 + j * 4);
		uint16_t delta = read_16u(data + offset + 2 + j * 4 + 2);
		if (delta) {
			array->records[j].anchor = otl_read_anchor(data, tableLength, offset + delta);
		} else {
			array->records[j].anchor.present = false;
			array->records[j].anchor.x = 0;
			array->records[j].anchor.y = 0;
		}
	}
	return array;
FAIL:
	DELETE(otl_delete_mark_array, array);
	return NULL;
}

bk_Block *bkFromAnchor(otl_Anchor a) {
	if (!a.present) return NULL;
	return bk_new_Block(b16, 1,            // format
	                    b16, (int16_t)a.x, // x
	                    b16, (int16_t)a.y, // y
	                    bkover);
}

// GPOS position value constants
const uint8_t FORMAT_DX = 1;
const uint8_t FORMAT_DY = 2;
const uint8_t FORMAT_DWIDTH = 4;
const uint8_t FORMAT_DHEIGHT = 8;

#define a(n) n + 0, n + 1, n + 1, n + 2             // 2 bits
#define b(n) a(n + 0), a(n + 1), a(n + 1), a(n + 2) // 4 bits
#define c(n) b(n + 0), b(n + 1), b(n + 1), b(n + 2) // 6 bits
#define d(n) c(n + 0), c(n + 1), c(n + 1), c(n + 2) // 8 bits
const uint8_t bits_in[0x100] = {d(0)};
#undef d
#undef c
#undef b
#undef a

// Length of a position value in bytes
uint8_t position_format_length(uint16_t format) {
	return bits_in[format & 0xFF] << 1;
}
otl_PositionValue position_zero() {
	otl_PositionValue v = {0.0, 0.0, 0.0, 0.0};
	return v;
}
// Read a position value from SFNT
otl_PositionValue read_gpos_value(font_file_pointer data, uint32_t tableLength, uint32_t offset, uint16_t format) {
	otl_PositionValue v = {0.0, 0.0, 0.0, 0.0};
	if (tableLength < offset + position_format_length(format)) return v;
	if (format & FORMAT_DX) { v.dx = read_16s(data + offset), offset += 2; };
	if (format & FORMAT_DY) { v.dy = read_16s(data + offset), offset += 2; };
	if (format & FORMAT_DWIDTH) { v.dWidth = read_16s(data + offset), offset += 2; };
	if (format & FORMAT_DHEIGHT) { v.dHeight = read_16s(data + offset), offset += 2; };
	return v;
}
json_value *gpos_dump_value(otl_PositionValue value) {
	json_value *v = json_object_new(4);
	if (value.dx) json_object_push(v, "dx", json_new_position(value.dx));
	if (value.dy) json_object_push(v, "dy", json_new_position(value.dy));
	if (value.dWidth) json_object_push(v, "dWidth", json_new_position(value.dWidth));
	if (value.dHeight) json_object_push(v, "dHeight", json_new_position(value.dHeight));
	return preserialize(v);
}
otl_PositionValue gpos_parse_value(json_value *pos) {
	otl_PositionValue v = {0.0, 0.0, 0.0, 0.0};
	if (!pos || pos->type != json_object) return v;
	v.dx = json_obj_getnum(pos, "dx");
	v.dy = json_obj_getnum(pos, "dy");
	v.dWidth = json_obj_getnum(pos, "dWidth");
	v.dHeight = json_obj_getnum(pos, "dHeight");
	return v;
}
// The required format of a position value
uint8_t required_position_format(otl_PositionValue v) {
	return (v.dx ? FORMAT_DX : 0) | (v.dy ? FORMAT_DY : 0) | (v.dWidth ? FORMAT_DWIDTH : 0) |
	       (v.dHeight ? FORMAT_DHEIGHT : 0);
}
// Write gpos position value
void write_gpos_value(caryll_buffer *buf, otl_PositionValue v, uint16_t format) {
	if (format & FORMAT_DX) bufwrite16b(buf, (int16_t)v.dx);
	if (format & FORMAT_DY) bufwrite16b(buf, (int16_t)v.dy);
	if (format & FORMAT_DWIDTH) bufwrite16b(buf, (int16_t)v.dWidth);
	if (format & FORMAT_DHEIGHT) bufwrite16b(buf, (int16_t)v.dHeight);
}

bk_Block *bk_gpos_value(otl_PositionValue v, uint16_t format) {
	bk_Block *b = bk_new_Block(bkover);
	if (format & FORMAT_DX) bk_push(b, b16, (int16_t)v.dx, bkover);
	if (format & FORMAT_DY) bk_push(b, b16, (int16_t)v.dy, bkover);
	if (format & FORMAT_DWIDTH) bk_push(b, b16, (int16_t)v.dWidth, bkover);
	if (format & FORMAT_DHEIGHT) bk_push(b, b16, (int16_t)v.dHeight, bkover);
	return b;
}
