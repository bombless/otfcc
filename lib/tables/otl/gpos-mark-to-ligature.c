#include "gpos-mark-to-ligature.h"
#include "gpos-common.h"

void delete_lig_attachment(otl_MarkToLigatureBase *att) {
	if (!att) return;
	if (att->anchors) {
		for (glyphid_t k = 0; k < att->componentCount; k++)
			free(att->anchors[k]);
		free(att->anchors);
	}
	free(att);
}

void otl_delete_gpos_markToLigature(otl_Subtable *_subtable) {
	if (_subtable) {
		subtable_gpos_markToLigature *subtable = &(_subtable->gpos_markToLigature);
		if (subtable->marks) { otl_delete_Coverage(subtable->marks); }
		if (subtable->markArray) { otl_delete_mark_array(subtable->markArray); }
		if (subtable->bases) {
			if (subtable->ligArray) {
				for (glyphid_t j = 0; j < subtable->bases->numGlyphs; j++) {
					delete_lig_attachment(subtable->ligArray[j]);
				}
				free(subtable->ligArray);
			}
			otl_delete_Coverage(subtable->bases);
		}
		free(_subtable);
	}
}

otl_Subtable *otl_read_gpos_markToLigature(font_file_pointer data, uint32_t tableLength, uint32_t offset) {
	otl_Subtable *_subtable;
	NEW(_subtable);
	subtable_gpos_markToLigature *subtable = &(_subtable->gpos_markToLigature);
	if (tableLength < offset + 12) goto FAIL;
	subtable->marks = otl_read_Coverage(data, tableLength, offset + read_16u(data + offset + 2));
	subtable->bases = otl_read_Coverage(data, tableLength, offset + read_16u(data + offset + 4));
	if (!subtable->marks || subtable->marks->numGlyphs == 0 || !subtable->bases || subtable->bases->numGlyphs == 0)
		goto FAIL;
	subtable->classCount = read_16u(data + offset + 6);
	uint32_t markArrayOffset = offset + read_16u(data + offset + 8);
	subtable->markArray = otl_read_mark_array(data, tableLength, markArrayOffset);
	if (!subtable->markArray || subtable->markArray->markCount != subtable->marks->numGlyphs) goto FAIL;

	uint32_t ligArrayOffset = offset + read_16u(data + offset + 10);
	checkLength(ligArrayOffset + 2 + 2 * subtable->bases->numGlyphs);
	if (read_16u(data + ligArrayOffset) != subtable->bases->numGlyphs) goto FAIL;
	NEW_N(subtable->ligArray, subtable->bases->numGlyphs);
	for (glyphid_t j = 0; j < subtable->bases->numGlyphs; j++) {
		subtable->ligArray[j] = NULL;
	}
	for (glyphid_t j = 0; j < subtable->bases->numGlyphs; j++) {
		uint32_t ligAttachOffset = ligArrayOffset + read_16u(data + ligArrayOffset + 2 + j * 2);
		NEW(subtable->ligArray[j]);
		subtable->ligArray[j]->anchors = NULL;
		checkLength(ligAttachOffset + 2);
		subtable->ligArray[j]->componentCount = read_16u(data + ligAttachOffset);

		checkLength(ligAttachOffset + 2 + 2 * subtable->ligArray[j]->componentCount * subtable->classCount);
		NEW_N(subtable->ligArray[j]->anchors, subtable->ligArray[j]->componentCount);

		uint32_t _offset = ligAttachOffset + 2;
		for (glyphid_t k = 0; k < subtable->ligArray[j]->componentCount; k++) {
			NEW_N(subtable->ligArray[j]->anchors[k], subtable->classCount);
			for (glyphclass_t m = 0; m < subtable->classCount; m++) {
				uint32_t anchorOffset = read_16u(data + _offset);
				if (anchorOffset) {
					subtable->ligArray[j]->anchors[k][m] =
					    otl_read_anchor(data, tableLength, ligAttachOffset + anchorOffset);
				} else {
					subtable->ligArray[j]->anchors[k][m].present = false;
					subtable->ligArray[j]->anchors[k][m].x = 0;
					subtable->ligArray[j]->anchors[k][m].y = 0;
				}
				_offset += 2;
			}
		}
	}
	goto OK;
FAIL:
	DELETE(otl_delete_gpos_markToLigature, _subtable);
OK:
	return _subtable;
}

json_value *otl_gpos_dump_markToLigature(otl_Subtable *st) {
	subtable_gpos_markToLigature *subtable = &(st->gpos_markToLigature);
	json_value *_subtable = json_object_new(3);
	json_value *_marks = json_object_new(subtable->marks->numGlyphs);
	json_value *_bases = json_object_new(subtable->bases->numGlyphs);
	for (glyphid_t j = 0; j < subtable->marks->numGlyphs; j++) {
		json_value *_mark = json_object_new(3);
		sds markClassName = sdscatfmt(sdsempty(), "ac_%i", subtable->markArray->records[j].markClass);
		json_object_push(_mark, "class", json_string_new_length((uint32_t)sdslen(markClassName), markClassName));
		sdsfree(markClassName);
		json_object_push(_mark, "x", json_integer_new(subtable->markArray->records[j].anchor.x));
		json_object_push(_mark, "y", json_integer_new(subtable->markArray->records[j].anchor.y));
		json_object_push(_marks, subtable->marks->glyphs[j].name, preserialize(_mark));
	}
	for (glyphid_t j = 0; j < subtable->bases->numGlyphs; j++) {
		otl_MarkToLigatureBase *base = subtable->ligArray[j];
		json_value *_base = json_array_new(base->componentCount);
		for (glyphid_t k = 0; k < base->componentCount; k++) {
			json_value *_bk = json_object_new(subtable->classCount);
			for (glyphclass_t m = 0; m < subtable->classCount; m++) {
				if (base->anchors[k][m].present) {
					json_value *_anchor = json_object_new(2);
					json_object_push(_anchor, "x", json_integer_new(base->anchors[k][m].x));
					json_object_push(_anchor, "y", json_integer_new(base->anchors[k][m].y));
					sds markClassName = sdscatfmt(sdsempty(), "ac_%i", m);
					json_object_push_length(_bk, (uint32_t)sdslen(markClassName), markClassName, _anchor);
					sdsfree(markClassName);
				}
			}
			json_array_push(_base, _bk);
		}
		json_object_push(_bases, subtable->bases->glyphs[j].name, preserialize(_base));
	}
	json_object_push(_subtable, "classCount", json_integer_new(subtable->classCount));
	json_object_push(_subtable, "marks", _marks);
	json_object_push(_subtable, "bases", _bases);
	return _subtable;
}

typedef struct {
	sds className;
	glyphclass_t classID;
	UT_hash_handle hh;
} classname_hash;
static void parseMarks(json_value *_marks, subtable_gpos_markToLigature *subtable, classname_hash **h) {
	NEW(subtable->marks);
	subtable->marks->numGlyphs = _marks->u.object.length;
	NEW_N(subtable->marks->glyphs, subtable->marks->numGlyphs);
	NEW(subtable->markArray);
	subtable->markArray->markCount = _marks->u.object.length;
	NEW_N(subtable->markArray->records, subtable->markArray->markCount);
	for (glyphid_t j = 0; j < _marks->u.object.length; j++) {
		char *gname = _marks->u.object.values[j].name;
		json_value *anchorRecord = _marks->u.object.values[j].value;
		subtable->marks->glyphs[j] = handle_fromName(sdsnewlen(gname, _marks->u.object.values[j].name_length));

		subtable->markArray->records[j].markClass = 0;
		subtable->markArray->records[j].anchor = otl_anchor_absent();

		if (!anchorRecord || anchorRecord->type != json_object) continue;
		json_value *_className = json_obj_get_type(anchorRecord, "class", json_string);
		if (!_className) continue;

		sds className = sdsnewlen(_className->u.string.ptr, _className->u.string.length);
		classname_hash *s;
		HASH_FIND_STR(*h, className, s);
		if (!s) {
			NEW(s);
			s->className = className;
			s->classID = HASH_COUNT(*h);
			HASH_ADD_STR(*h, className, s);
		} else {
			sdsfree(className);
		}
		subtable->markArray->records[j].markClass = s->classID;
		subtable->markArray->records[j].anchor.present = true;
		subtable->markArray->records[j].anchor.x = json_obj_getnum(anchorRecord, "x");
		subtable->markArray->records[j].anchor.y = json_obj_getnum(anchorRecord, "y");
	}
}
static void parseBases(json_value *_bases, subtable_gpos_markToLigature *subtable, classname_hash **h) {
	glyphclass_t classCount = HASH_COUNT(*h);
	NEW(subtable->bases);
	subtable->bases->numGlyphs = _bases->u.object.length;
	NEW_N(subtable->bases->glyphs, subtable->bases->numGlyphs);
	NEW_N(subtable->ligArray, _bases->u.object.length);
	for (glyphid_t j = 0; j < _bases->u.object.length; j++) {
		subtable->bases->glyphs[j] =
		    handle_fromName(sdsnewlen(_bases->u.object.values[j].name, _bases->u.object.values[j].name_length));
		NEW(subtable->ligArray[j]);
		subtable->ligArray[j]->componentCount = 0;
		subtable->ligArray[j]->anchors = NULL;

		json_value *baseRecord = _bases->u.object.values[j].value;
		if (!baseRecord || baseRecord->type != json_array) continue;
		subtable->ligArray[j]->componentCount = baseRecord->u.array.length;

		NEW_N(subtable->ligArray[j]->anchors, subtable->ligArray[j]->componentCount);

		for (glyphid_t k = 0; k < subtable->ligArray[j]->componentCount; k++) {
			json_value *_componentRecord = baseRecord->u.array.values[k];
			NEW_N(subtable->ligArray[j]->anchors[k], classCount);
			for (glyphclass_t m = 0; m < classCount; m++) {
				subtable->ligArray[j]->anchors[k][m] = otl_anchor_absent();
			}
			if (!_componentRecord || _componentRecord->type != json_object) { continue; }
			for (glyphclass_t m = 0; m < _componentRecord->u.object.length; m++) {
				sds className = sdsnewlen(_componentRecord->u.object.values[m].name,
				                          _componentRecord->u.object.values[m].name_length);
				classname_hash *s;
				HASH_FIND_STR(*h, className, s);
				if (!s) goto NEXT;
				subtable->ligArray[j]->anchors[k][s->classID] =
				    otl_parse_anchor(_componentRecord->u.object.values[m].value);

			NEXT:
				sdsfree(className);
			}
		}
	}
}
otl_Subtable *otl_gpos_parse_markToLigature(json_value *_subtable) {
	json_value *_marks = json_obj_get_type(_subtable, "marks", json_object);
	json_value *_bases = json_obj_get_type(_subtable, "bases", json_object);
	if (!_marks || !_bases) return NULL;
	otl_Subtable *st;
	NEW(st);
	classname_hash *h = NULL;
	parseMarks(_marks, &(st->gpos_markToLigature), &h);
	st->gpos_markToLigature.classCount = HASH_COUNT(h);
	parseBases(_bases, &(st->gpos_markToLigature), &h);

	classname_hash *s, *tmp;
	HASH_ITER(hh, h, s, tmp) {
		HASH_DEL(h, s);
		sdsfree(s->className);
		free(s);
	}

	return st;
}

caryll_buffer *caryll_build_gpos_markToLigature(otl_Subtable *_subtable) {
	subtable_gpos_markToLigature *subtable = &(_subtable->gpos_markToLigature);
	bk_Block *root = bk_new_Block(b16, 1,                                                          // format
	                              p16, bk_newBlockFromBuffer(otl_build_Coverage(subtable->marks)), // markCoverage
	                              p16, bk_newBlockFromBuffer(otl_build_Coverage(subtable->bases)), // baseCoverage
	                              b16, subtable->classCount,                                       // classCont
	                              bkover);

	bk_Block *markArray = bk_new_Block(b16, subtable->marks->numGlyphs, // markCount
	                                   bkover);
	for (glyphid_t j = 0; j < subtable->marks->numGlyphs; j++) {
		bk_push(markArray,                                                 // markArray item
		        b16, subtable->markArray->records[j].markClass,            // markClass
		        p16, bkFromAnchor(subtable->markArray->records[j].anchor), // Anchor
		        bkover);
	}

	bk_Block *ligatureArray = bk_new_Block(b16, subtable->bases->numGlyphs, bkover);
	for (glyphid_t j = 0; j < subtable->bases->numGlyphs; j++) {
		bk_Block *attach = bk_new_Block(b16, subtable->ligArray[j]->componentCount, // componentCount
		                                bkover);
		for (glyphid_t k = 0; k < subtable->ligArray[j]->componentCount; k++) {
			for (glyphclass_t m = 0; m < subtable->classCount; m++) {
				bk_push(attach, p16, bkFromAnchor(subtable->ligArray[j]->anchors[k][m]), bkover);
			}
		}
		bk_push(ligatureArray, p16, attach, bkover);
	}

	bk_push(root, p16, markArray, p16, ligatureArray, bkover);

	return bk_build_Block(root);
}
