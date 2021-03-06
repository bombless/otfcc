#include "otl.h"

static const char SCRIPT_LANGUAGE_SEPARATOR = '_';

typedef enum { LOOKUP_ORDER_FORCE, LOOKUP_ORDER_FILE } lookup_order_type;

typedef struct {
	char *name;
	bool alias;
	otl_Lookup *lookup;
	UT_hash_handle hh;
	lookup_order_type orderType;
	uint16_t orderVal;
} lookup_hash;

typedef struct {
	char *name;
	bool alias;
	otl_Feature *feature;
	UT_hash_handle hh;
} feature_hash;

typedef struct {
	char *name;
	otl_LanguageSystem *script;
	UT_hash_handle hh;
} language_hash;

void caryll_delete_lookup(otl_Lookup *lookup);
otl_Subtable *table_read_otl_subtable(font_file_pointer data, uint32_t tableLength, uint32_t subtableOffset,
                                      otl_LookupType lookupType);
static void _dump_lookup(otl_Lookup *lookup, json_value *dump);
static bool _parse_lookup(json_value *lookup, char *lookupName, lookup_hash **lh);
static bool _build_subtable(otl_Lookup *lookup, caryll_buffer ***subtables, size_t *lastOffset);

// COMMON PART
table_OTL *table_new_otl() {
	table_OTL *table;
	NEW(table);
	table->languageCount = 0;
	table->languages = NULL;
	table->featureCount = 0;
	table->features = NULL;
	table->lookupCount = 0;
	table->lookups = NULL;
	table->lookupAliasesCount = 0;
	table->lookupAliases = NULL;
	return table;
}

void table_delete_otl(table_OTL *table) {
	if (!table) return;
	if (table->languages) {
		for (tableid_t j = 0; j < table->languageCount; j++) {
			if (table->languages[j]->name) sdsfree(table->languages[j]->name);
			if (table->languages[j]->features) free(table->languages[j]->features);
			free(table->languages[j]);
		}
		free(table->languages);
	}
	if (table->features) {
		for (tableid_t j = 0; j < table->featureCount; j++) {
			if (table->features[j]->name) sdsfree(table->features[j]->name);
			if (table->features[j]->lookups) free(table->features[j]->lookups);
			free(table->features[j]);
		}
		free(table->features);
	}
	if (table->lookups) {
		for (tableid_t j = 0; j < table->lookupCount; j++) {
			caryll_delete_lookup(table->lookups[j]);
		}
		free(table->lookups);
	}
	if (table->lookupAliases) {
		for (tableid_t j = 0; j < table->lookupAliasesCount; j++) {
			sdsfree(table->lookupAliases[j].from);
			sdsfree(table->lookupAliases[j].to);
		}
		free(table->lookupAliases);
	}
	free(table);
}

static void parseLanguage(font_file_pointer data, uint32_t tableLength, uint32_t base, otl_LanguageSystem *lang,
                          tableid_t featureCount, otl_Feature **features) {
	checkLength(base + 6);
	tableid_t rid = read_16u(data + base + 2);
	if (rid < featureCount) {
		lang->requiredFeature = features[rid];
	} else {
		lang->requiredFeature = NULL;
	}
	lang->featureCount = read_16u(data + base + 4);
	checkLength(base + 6 + lang->featureCount * 2);

	NEW_N(lang->features, lang->featureCount);
	for (tableid_t j = 0; j < lang->featureCount; j++) {
		tableid_t featureIndex = read_16u(data + base + 6 + 2 * j);
		if (featureIndex < featureCount) {
			lang->features[j] = features[featureIndex];
		} else {
			lang->features[j] = NULL;
		}
	}
	return;
FAIL:
	if (lang->features) free(lang->features);
	lang->featureCount = 0;
	lang->requiredFeature = NULL;
	return;
}

static table_OTL *table_read_otl_common(font_file_pointer data, uint32_t tableLength, otl_LookupType lookup_type_base) {
	table_OTL *table = table_new_otl();
	if (!table) goto FAIL;
	checkLength(10);
	uint32_t scriptListOffset = read_16u(data + 4);
	checkLength(scriptListOffset + 2);
	uint32_t featureListOffset = read_16u(data + 6);
	checkLength(featureListOffset + 2);
	uint32_t lookupListOffset = read_16u(data + 8);
	checkLength(lookupListOffset + 2);

	// parse lookup list
	{
		tableid_t lookupCount = read_16u(data + lookupListOffset);
		checkLength(lookupListOffset + 2 + lookupCount * 2);
		otl_Lookup **lookups;
		NEW_N(lookups, lookupCount);
		for (tableid_t j = 0; j < lookupCount; j++) {
			NEW(lookups[j]);
			lookups[j]->name = NULL;
			lookups[j]->_offset = lookupListOffset + read_16u(data + lookupListOffset + 2 + 2 * j);
			checkLength(lookups[j]->_offset + 6);
			lookups[j]->type = read_16u(data + lookups[j]->_offset) + lookup_type_base;
		}
		table->lookupCount = lookupCount;
		table->lookups = lookups;
	}

	// parse feature list
	{
		tableid_t featureCount = read_16u(data + featureListOffset);
		checkLength(featureListOffset + 2 + featureCount * 6);
		otl_Feature **features;
		NEW_N(features, featureCount);
		tableid_t lnk = 0;
		for (tableid_t j = 0; j < featureCount; j++) {
			otl_Feature *feature;
			NEW(feature);
			features[j] = feature;
			uint32_t tag = read_32u(data + featureListOffset + 2 + j * 6);
			features[j]->name = sdscatprintf(sdsempty(), "%c%c%c%c_%05d", (tag >> 24) & 0xFF, (tag >> 16) & 0xFF,
			                                 (tag >> 8) & 0xff, tag & 0xff, j);
			uint32_t featureOffset = featureListOffset + read_16u(data + featureListOffset + 2 + j * 6 + 4);

			checkLength(featureOffset + 4);
			tableid_t lookupCount = read_16u(data + featureOffset + 2);
			checkLength(featureOffset + 4 + lookupCount * 2);
			features[j]->lookupCount = lookupCount;
			NEW_N(features[j]->lookups, lookupCount);
			for (tableid_t k = 0; k < lookupCount; k++) {
				tableid_t lookupid = read_16u(data + featureOffset + 4 + k * 2);
				if (lookupid < table->lookupCount) {
					features[j]->lookups[k] = table->lookups[lookupid];
					if (!features[j]->lookups[k]->name) {
						features[j]->lookups[k]->name =
						    sdscatprintf(sdsempty(), "lookup_%c%c%c%c_%d", (tag >> 24) & 0xFF, (tag >> 16) & 0xFF,
						                 (tag >> 8) & 0xff, tag & 0xff, lnk++);
					}
				}
			}
		}
		table->featureCount = featureCount;
		table->features = features;
	}

	// parse script list
	{
		tableid_t scriptCount = read_16u(data + scriptListOffset);
		checkLength(scriptListOffset + 2 + 6 * scriptCount);

		uint32_t nLanguageCombinations = 0;
		for (tableid_t j = 0; j < scriptCount; j++) {
			uint32_t scriptOffset = scriptListOffset + read_16u(data + scriptListOffset + 2 + 6 * j + 4);
			checkLength(scriptOffset + 4);

			tableid_t defaultLangSystem = read_16u(data + scriptOffset);
			nLanguageCombinations += (defaultLangSystem ? 1 : 0) + read_16u(data + scriptOffset + 2);
		}

		table->languageCount = nLanguageCombinations;
		otl_LanguageSystem **languages;
		NEW_N(languages, nLanguageCombinations);

		tableid_t currentLang = 0;
		for (tableid_t j = 0; j < scriptCount; j++) {
			uint32_t tag = read_32u(data + scriptListOffset + 2 + 6 * j);
			uint32_t scriptOffset = scriptListOffset + read_16u(data + scriptListOffset + 2 + 6 * j + 4);
			tableid_t defaultLangSystem = read_16u(data + scriptOffset);
			if (defaultLangSystem) {
				NEW(languages[currentLang]);
				languages[currentLang]->name =
				    sdscatprintf(sdsempty(), "%c%c%c%c%cDFLT", (tag >> 24) & 0xFF, (tag >> 16) & 0xFF,
				                 (tag >> 8) & 0xff, tag & 0xff, SCRIPT_LANGUAGE_SEPARATOR);
				parseLanguage(data, tableLength, scriptOffset + defaultLangSystem, languages[currentLang],
				              table->featureCount, table->features);
				currentLang += 1;
			}
			tableid_t langSysCount = read_16u(data + scriptOffset + 2);
			for (tableid_t k = 0; k < langSysCount; k++) {
				uint32_t langTag = read_32u(data + scriptOffset + 4 + 6 * k);
				tableid_t langSys = read_16u(data + scriptOffset + 4 + 6 * k + 4);
				NEW(languages[currentLang]);
				languages[currentLang]->name =
				    sdscatprintf(sdsempty(), "%c%c%c%c%c%c%c%c%c", (tag >> 24) & 0xFF, (tag >> 16) & 0xFF,
				                 (tag >> 8) & 0xff, tag & 0xff, SCRIPT_LANGUAGE_SEPARATOR, (langTag >> 24) & 0xFF,
				                 (langTag >> 16) & 0xFF, (langTag >> 8) & 0xff, langTag & 0xff);
				parseLanguage(data, tableLength, scriptOffset + langSys, languages[currentLang], table->featureCount,
				              table->features);
				currentLang += 1;
			}
		}

		table->languages = languages;
	}
	// name all lookups
	for (tableid_t j = 0; j < table->lookupCount; j++) {
		if (!table->lookups[j]->name)
			table->lookups[j]->name = sdscatprintf(sdsempty(), "lookup_%02x_%d", table->lookups[j]->type, j);
	}
	return table;
FAIL:
	if (table) table_delete_otl(table);
	return NULL;
}

static void table_read_otl_lookup(font_file_pointer data, uint32_t tableLength, otl_Lookup *lookup) {
	lookup->flags = read_16u(data + lookup->_offset + 2);
	lookup->subtableCount = read_16u(data + lookup->_offset + 4);
	if (!lookup->subtableCount || tableLength < lookup->_offset + 6 + 2 * lookup->subtableCount) {
		lookup->type = otl_type_unknown;
		lookup->subtableCount = 0;
		lookup->subtables = NULL;
		return;
	}
	NEW_N(lookup->subtables, lookup->subtableCount);
	for (tableid_t j = 0; j < lookup->subtableCount; j++) {
		uint32_t subtableOffset = lookup->_offset + read_16u(data + lookup->_offset + 6 + j * 2);
		lookup->subtables[j] = table_read_otl_subtable(data, tableLength, subtableOffset, lookup->type);
	}
	if (lookup->type == otl_type_gsub_extend || lookup->type == otl_type_gpos_extend) {
		lookup->type = 0;
		for (tableid_t j = 0; j < lookup->subtableCount; j++) {
			if (lookup->subtables[j]) {
				lookup->type = lookup->subtables[j]->extend.type;
				break;
			}
		}
		if (lookup->type) {
			for (tableid_t j = 0; j < lookup->subtableCount; j++) {
				if (lookup->subtables[j] && lookup->subtables[j]->extend.type == lookup->type) {
					// this subtable is valid
					otl_Subtable *st = lookup->subtables[j]->extend.subtable;
					FREE(lookup->subtables[j]);
					lookup->subtables[j] = st;
				} else if (lookup->subtables[j]) {
					// delete this subtable
					otl_Lookup *temp;
					NEW(temp);
					temp->type = lookup->subtables[j]->extend.type;
					temp->subtableCount = 1;
					NEW_N(temp->subtables, 1);
					temp->subtables[0] = lookup->subtables[j]->extend.subtable;
					DELETE(caryll_delete_lookup, temp);
					FREE(lookup->subtables[j]);
				}
			}
		} else {
			FREE(lookup->subtables);
			lookup->subtableCount = 0;
			return;
		}
	}
	if (lookup->type == otl_type_gsub_context) lookup->type = otl_type_gsub_chaining;
	if (lookup->type == otl_type_gpos_context) lookup->type = otl_type_gpos_chaining;
}

table_OTL *table_read_otl(caryll_Packet packet, uint32_t tag) {
	table_OTL *otl = NULL;
	FOR_TABLE(tag, table) {
		font_file_pointer data = table.data;
		uint32_t length = table.length;
		otl = table_read_otl_common(
		    data, length,
		    (tag == 'GSUB' ? otl_type_gsub_unknown : tag == 'GPOS' ? otl_type_gpos_unknown : otl_type_unknown));
		if (!otl) goto FAIL;
		for (tableid_t j = 0; j < otl->lookupCount; j++) {
			table_read_otl_lookup(data, length, otl->lookups[j]);
		}
		return otl;
	FAIL:
		if (otl) table_delete_otl(otl);
		otl = NULL;
	}
	return NULL;
}

static const char *lookupFlagsLabels[] = {"rightToLeft", "ignoreBases", "ignoreLigatures", "ignoreMarks", NULL};

static void _declare_lookup_dumper(otl_LookupType llt, const char *lt, json_value *(*dumper)(otl_Subtable *st),
                                   otl_Lookup *lookup, json_value *dump) {
	if (lookup->type == llt) {
		json_object_push(dump, "type", json_string_new(lt));
		json_object_push(dump, "flags", caryll_dump_flags(lookup->flags, lookupFlagsLabels));
		if (lookup->flags >> 8) { json_object_push(dump, "markAttachmentType", json_integer_new(lookup->flags >> 8)); }
		json_value *subtables = json_array_new(lookup->subtableCount);
		for (tableid_t j = 0; j < lookup->subtableCount; j++)
			if (lookup->subtables[j]) { json_array_push(subtables, dumper(lookup->subtables[j])); }
		json_object_push(dump, "subtables", subtables);
	}
}

void table_dump_otl(table_OTL *table, json_value *root, const caryll_Options *options, const char *tag) {
	if (!table || !table->languages || !table->lookups || !table->features) return;
	if (options->verbose) fprintf(stderr, "Dumping %s.\n", tag);

	json_value *otl = json_object_new(3);
	{
		// dump script list
		json_value *languages = json_object_new(table->languageCount);
		for (tableid_t j = 0; j < table->languageCount; j++) {
			json_value *language = json_object_new(5);
			if (table->languages[j]->requiredFeature) {
				json_object_push(language, "requiredFeature",
				                 json_string_new(table->languages[j]->requiredFeature->name));
			}
			json_value *features = json_array_new(table->languages[j]->featureCount);
			for (tableid_t k = 0; k < table->languages[j]->featureCount; k++)
				if (table->languages[j]->features[k]) {
					json_array_push(features, json_string_new(table->languages[j]->features[k]->name));
				}
			json_object_push(language, "features", preserialize(features));
			json_object_push(languages, table->languages[j]->name, language);
		}
		json_object_push(otl, "languages", languages);
	}
	{
		// dump feature list
		json_value *features = json_object_new(table->featureCount);
		for (tableid_t j = 0; j < table->featureCount; j++) {
			json_value *feature = json_array_new(table->features[j]->lookupCount);
			for (tableid_t k = 0; k < table->features[j]->lookupCount; k++)
				if (table->features[j]->lookups[k]) {
					json_array_push(feature, json_string_new(table->features[j]->lookups[k]->name));
				}
			json_object_push(features, table->features[j]->name, preserialize(feature));
		}
		json_object_push(otl, "features", features);
	}
	{
		// dump lookups
		json_value *lookups = json_object_new(table->lookupCount);
		json_value *lookupOrder = json_array_new(table->lookupCount);
		for (tableid_t j = 0; j < table->lookupCount; j++) {
			json_value *lookup = json_object_new(5);
			_dump_lookup(table->lookups[j], lookup);
			json_object_push(lookups, table->lookups[j]->name, lookup);
			json_array_push(lookupOrder, json_string_new(table->lookups[j]->name));
		}
		json_object_push(otl, "lookups", lookups);
		json_object_push(otl, "lookupOrder", lookupOrder);
	}
	json_object_push(root, tag, otl);
}

static bool _declareLookupParser(const char *lt, otl_LookupType llt, otl_Subtable *(*parser)(json_value *),
                                 json_value *_lookup, char *lookupName, lookup_hash **lh) {
	json_value *type = json_obj_get_type(_lookup, "type", json_string);
	if (!type || strcmp(type->u.string.ptr, lt)) return false;
	lookup_hash *item = NULL;
	HASH_FIND_STR(*lh, lookupName, item);
	if (item) return false;
	json_value *_subtables = json_obj_get_type(_lookup, "subtables", json_array);
	if (!_subtables) return false;

	otl_Lookup *lookup;
	NEW(lookup);
	lookup->name = NULL;
	lookup->type = llt;
	lookup->flags = caryll_parse_flags(json_obj_get(_lookup, "flags"), lookupFlagsLabels);
	uint16_t markAttachmentType = json_obj_getint(_lookup, "markAttachmentType");
	if (markAttachmentType) { lookup->flags |= markAttachmentType << 8; }
	lookup->subtableCount = _subtables->u.array.length;
	NEW_N(lookup->subtables, lookup->subtableCount);
	tableid_t jj = 0;
	for (tableid_t j = 0; j < lookup->subtableCount; j++) {
		json_value *_subtable = _subtables->u.array.values[j];
		if (_subtable && _subtable->type == json_object) {
			otl_Subtable *_st = parser(_subtable);
			if (_st) { lookup->subtables[jj++] = _st; }
		}
	}
	lookup->subtableCount = jj;

	NEW(item);
	item->name = sdsnew(lookupName);
	item->alias = false;
	lookup->name = sdsdup(item->name);
	item->lookup = lookup;
	item->orderType = LOOKUP_ORDER_FILE;
	item->orderVal = HASH_COUNT(*lh);
	HASH_ADD_STR(*lh, name, item);

	return true;
}

static void feature_merger_activate(json_value *d, const bool sametag, const char *objtype,
                                    const caryll_Options *options) {
	for (uint32_t j = 0; j < d->u.object.length; j++) {
		json_value *jthis = d->u.object.values[j].value;
		char *kthis = d->u.object.values[j].name;
		uint32_t nkthis = d->u.object.values[j].name_length;
		if (jthis->type != json_array && jthis->type != json_object) continue;
		for (uint32_t k = j + 1; k < d->u.object.length; k++) {
			json_value *jthat = d->u.object.values[k].value;
			char *kthat = d->u.object.values[k].name;
			if (json_ident(jthis, jthat) && (sametag ? strncmp(kthis, kthat, 4) == 0 : true)) {
				json_value_free(jthat);
				d->u.object.values[k].value = json_string_new_length(nkthis, kthis);
				if (options->verbose) {
					fprintf(stderr, "[OTFCC-fea] Merged duplicate %s '%s' into '%s'.\n", objtype, kthat, kthis);
				}
			}
		}
	}
}

static void replace_aliased_lookup_names(json_value *features, lookup_hash *lh) {
	for (uint32_t j = 0; j < features->u.object.length; j++) {
		json_value *_feature = features->u.object.values[j].value;
		if (_feature->type != json_array) continue;
		for (tableid_t k = 0; k < _feature->u.array.length; k++) {
			json_value *term = _feature->u.array.values[k];
			if (term->type != json_string) continue;

			lookup_hash *item = NULL;
			HASH_FIND_STR(lh, term->u.string.ptr, item);
			if (item && item->alias) {
				json_value_free(term);
				term = _feature->u.array.values[k] = json_string_new(item->lookup->name);
			}
		}
	}
}

static feature_hash *figureOutFeaturesFromJSON(json_value *features, lookup_hash *lh, const char *tag,
                                               const caryll_Options *options) {
	feature_hash *fh = NULL;
	// Replace aliased lookup names
	replace_aliased_lookup_names(features, lh);
	// Remove duplicates
	if (options->merge_features) { feature_merger_activate(features, true, "feature", options); }
	// Resolve features
	for (uint32_t j = 0; j < features->u.object.length; j++) {
		char *featureName = features->u.object.values[j].name;
		json_value *_feature = features->u.object.values[j].value;
		if (_feature->type == json_array) {
			tableid_t nal = 0;
			otl_Lookup **al;
			NEW_N(al, _feature->u.array.length);
			for (tableid_t k = 0; k < _feature->u.array.length; k++) {
				json_value *term = _feature->u.array.values[k];
				if (term->type != json_string) continue;
				lookup_hash *item = NULL;
				HASH_FIND_STR(lh, term->u.string.ptr, item);
				if (item) { al[nal++] = item->lookup; }
			}
			if (nal > 0) {
				feature_hash *s = NULL;
				HASH_FIND_STR(fh, featureName, s);
				if (!s) {
					NEW(s);
					s->name = sdsnew(featureName);
					s->alias = false;
					NEW(s->feature);
					s->feature->name = sdsdup(s->name);
					s->feature->lookupCount = nal;
					s->feature->lookups = al;
					HASH_ADD_STR(fh, name, s);
				} else {
					fprintf(stderr, "[OTFCC-fea] Duplicate feature for [%s/%s]. This feature will "
					                "be ignored.\n",
					        tag, featureName);
					FREE(al);
				}
			} else {
				fprintf(stderr, "[OTFCC-fea] There is no valid lookup "
				                "assignments for [%s/%s]. This feature will be "
				                "ignored.\n",
				        tag, featureName);
				FREE(al);
			}
		} else if (_feature->type == json_string) {
			feature_hash *s = NULL;
			char *target = _feature->u.string.ptr;
			HASH_FIND_STR(fh, target, s);
			if (s) {
				feature_hash *dup;
				NEW(dup);
				dup->alias = true;
				dup->name = sdsnew(featureName);
				dup->feature = s->feature;
				HASH_ADD_STR(fh, name, dup);
			}
		}
	}
	return fh;
}
bool isValidLanguageName(const char *name, const size_t length) {
	return length == 9 && name[4] == SCRIPT_LANGUAGE_SEPARATOR;
}
static language_hash *figureOutLanguagesFromJson(json_value *languages, feature_hash *fh, const char *tag,
                                                 const caryll_Options *options) {
	language_hash *sh = NULL;
	// languages
	for (uint32_t j = 0; j < languages->u.object.length; j++) {
		char *languageName = languages->u.object.values[j].name;
		size_t languageNameLen = languages->u.object.values[j].name_length;
		json_value *_language = languages->u.object.values[j].value;
		if (isValidLanguageName(languageName, languageNameLen) && _language->type == json_object) {
			otl_Feature *requiredFeature = NULL;
			json_value *_rf = json_obj_get_type(_language, "requiredFeature", json_string);
			if (_rf) {
				// required feature term
				feature_hash *rf = NULL;
				HASH_FIND_STR(fh, _rf->u.string.ptr, rf);
				if (rf) { requiredFeature = rf->feature; }
			}

			tableid_t naf = 0;
			otl_Feature **af = NULL;
			json_value *_features = json_obj_get_type(_language, "features", json_array);
			if (_features) {
				NEW_N(af, _features->u.array.length);
				for (tableid_t k = 0; k < _features->u.array.length; k++) {
					json_value *term = _features->u.array.values[k];
					if (term->type == json_string) {
						feature_hash *item = NULL;
						HASH_FIND_STR(fh, term->u.string.ptr, item);
						if (item) { af[naf++] = item->feature; }
					}
				}
			}
			if (requiredFeature || (af && naf > 0)) {
				language_hash *s = NULL;
				HASH_FIND_STR(sh, languageName, s);
				if (!s) {
					NEW(s);
					s->name = sdsnew(languageName);
					NEW(s->script);
					s->script->name = sdsdup(s->name);
					s->script->requiredFeature = requiredFeature;
					s->script->featureCount = naf;
					s->script->features = af;
					HASH_ADD_STR(sh, name, s);
				} else {
					fprintf(stderr, "[OTFCC-fea] Duplicate language item [%s/%s]. This language "
					                "term will be ignored.\n",
					        tag, languageName);
					if (af) { FREE(af); }
				}
			} else {
				fprintf(stderr, "[OTFCC-fea] There is no valid feature "
				                "assignments for [%s/%s]. This language term "
				                "will be ignored.\n",
				        tag, languageName);

				if (af) { FREE(af); }
			}
		}
	}
	return sh;
}

static lookup_hash *figureOutLookupsFromJSON(json_value *lookups, const caryll_Options *options) {
	lookup_hash *lh = NULL;
	if (options->merge_lookups) { feature_merger_activate(lookups, false, "lookup", options); }

	for (uint32_t j = 0; j < lookups->u.object.length; j++) {
		char *lookupName = lookups->u.object.values[j].name;
		if (lookups->u.object.values[j].value->type == json_object) {
			bool parsed = _parse_lookup(lookups->u.object.values[j].value, lookupName, &lh);
			if (!parsed) { fprintf(stderr, "[OTFCC-fea] Ignoring unknown or unsupported lookup %s.\n", lookupName); }
			FREE(lookups->u.object.values[j].value);
		} else if (lookups->u.object.values[j].value->type == json_string) {
			char *thatname = lookups->u.object.values[j].value->u.string.ptr;
			lookup_hash *s = NULL;
			HASH_FIND_STR(lh, thatname, s);
			if (s) {
				lookup_hash *dup;
				NEW(dup);
				dup->name = sdsnew(lookupName);
				dup->alias = true;
				dup->lookup = s->lookup;
				dup->orderType = LOOKUP_ORDER_FILE;
				dup->orderVal = HASH_COUNT(lh);
				HASH_ADD_STR(lh, name, dup);
			}
		}
	}
	return lh;
}
static int by_lookup_order(lookup_hash *a, lookup_hash *b) {
	if (a->orderType == b->orderType) {
		return a->orderVal - b->orderVal;
	} else {
		return a->orderType - b->orderType;
	}
}
static int by_feature_name(feature_hash *a, feature_hash *b) {
	return strcmp(a->name, b->name);
}
static int by_language_name(language_hash *a, language_hash *b) {
	return strcmp(a->name, b->name);
}
table_OTL *table_parse_otl(json_value *root, const caryll_Options *options, const char *tag) {
	table_OTL *otl = NULL;
	json_value *table = json_obj_get_type(root, tag, json_object);
	if (!table) goto FAIL;

	if (options->verbose) fprintf(stderr, "Parsing %s.\n", tag);
	otl = table_new_otl();
	json_value *languages = json_obj_get_type(table, "languages", json_object);
	json_value *features = json_obj_get_type(table, "features", json_object);
	json_value *lookups = json_obj_get_type(table, "lookups", json_object);
	if (!languages || !features || !lookups) goto FAIL;

	lookup_hash *lh = figureOutLookupsFromJSON(lookups, options);
	json_value *lookupOrder = json_obj_get_type(table, "lookupOrder", json_array);
	if (lookupOrder) {
		for (tableid_t j = 0; j < lookupOrder->u.array.length; j++) {
			json_value *_ln = lookupOrder->u.array.values[j];
			if (_ln && _ln->type == json_string) {
				lookup_hash *item = NULL;
				HASH_FIND_STR(lh, _ln->u.string.ptr, item);
				if (item) {
					item->orderType = LOOKUP_ORDER_FORCE;
					item->orderVal = j;
				}
			}
		}
	}
	HASH_SORT(lh, by_lookup_order);
	feature_hash *fh = figureOutFeaturesFromJSON(features, lh, tag, options);
	HASH_SORT(fh, by_feature_name);
	language_hash *sh = figureOutLanguagesFromJson(languages, fh, tag, options);
	HASH_SORT(sh, by_language_name);
	if (!HASH_COUNT(lh) || !HASH_COUNT(fh) || !HASH_COUNT(sh)) goto FAIL;

	{
		lookup_hash *s, *tmp;
		otl->lookupCount = HASH_COUNT(lh);
		otl->lookupAliasesCount = HASH_COUNT(lh);
		NEW_N(otl->lookups, otl->lookupCount);
		NEW_N(otl->lookupAliases, otl->lookupAliasesCount);
		tableid_t j = 0;
		tableid_t ja = 0;
		HASH_ITER(hh, lh, s, tmp) {
			if (!s->alias) {
				otl->lookups[j] = s->lookup;
				j++;
			} else {
				otl->lookupAliases[ja].from = sdsdup(s->name);
				otl->lookupAliases[ja].to = sdsdup(s->lookup->name);
				ja++;
			}
			HASH_DEL(lh, s);
			sdsfree(s->name);
			free(s);
		}
		otl->lookupCount = j;
		otl->lookupAliasesCount = ja;
	}
	{
		feature_hash *s, *tmp;
		otl->featureCount = HASH_COUNT(fh);
		NEW_N(otl->features, otl->featureCount);
		tableid_t j = 0;
		HASH_ITER(hh, fh, s, tmp) {
			if (!s->alias) {
				otl->features[j] = s->feature;
				j++;
			}
			HASH_DEL(fh, s);
			sdsfree(s->name);
			free(s);
		}
		otl->featureCount = j;
	}
	{
		language_hash *s, *tmp;
		otl->languageCount = HASH_COUNT(sh);
		NEW_N(otl->languages, otl->languageCount);
		tableid_t j = 0;
		HASH_ITER(hh, sh, s, tmp) {
			otl->languages[j] = s->script;
			HASH_DEL(sh, s);
			sdsfree(s->name);
			free(s);
			j++;
		}
	}
	return otl;
FAIL:
	if (otl) {
		fprintf(stderr, "[OTFCC-fea] Ignoring invalid or incomplete OTL table %s.\n", tag);
		table_delete_otl(otl);
	}
	return NULL;
}

static bool _declare_lookup_writer(otl_LookupType type, caryll_buffer *(*fn)(otl_Subtable *_subtable),
                                   otl_Lookup *lookup, caryll_buffer ***subtables, size_t *lastOffset) {
	if (lookup->type == type) {
		NEW_N(*subtables, lookup->subtableCount);
		for (tableid_t j = 0; j < lookup->subtableCount; j++) {
			caryll_buffer *buf = fn(lookup->subtables[j]);
			(*subtables)[j] = buf;
			*lastOffset += buf->size;
		}
		return true;
	}
	return false;
}

// When writing lookups, otfcc will try to maintain everything correctly.
// That is, we will use extended layout lookups automatically when the
// offsets are too large.
static bk_Block *writeOTLLookups(table_OTL *table, const caryll_Options *options, const char *tag) {
	caryll_buffer ***subtables;
	NEW_N(subtables, table->lookupCount);
	bool *lookupWritten;
	NEW_N(lookupWritten, table->lookupCount);
	size_t lastOffset = 0;
	for (tableid_t j = 0; j < table->lookupCount; j++) {
		if (options->verbose) { fprintf(stderr, "    Writing lookup %s\n", table->lookups[j]->name); }
		subtables[j] = NULL;
		lookupWritten[j] = _build_subtable(table->lookups[j], &(subtables[j]), &lastOffset);
	}
	size_t headerSize = 2 + 2 * table->lookupCount;
	for (tableid_t j = 0; j < table->lookupCount; j++) {
		if (lookupWritten[j]) { headerSize += 6 + 2 * table->lookups[j]->subtableCount; }
	}
	bool useExtended = lastOffset >= 0xFF00 - headerSize;
	if (useExtended) {
		if (options->verbose) { fprintf(stderr, "[OTFCC-fea] Using extended OpenType table layout for %s.\n", tag); }
		for (tableid_t j = 0; j < table->lookupCount; j++) {
			if (lookupWritten[j]) { headerSize += 8 * table->lookups[j]->subtableCount; }
		}
	}

	bk_Block *root = bk_new_Block(b16, table->lookupCount, // LookupCount
	                              bkover);
	for (tableid_t j = 0; j < table->lookupCount; j++) {
		if (!lookupWritten[j]) {
			fprintf(stderr, "Lookup %s not written.\n", table->lookups[j]->name);
			continue;
		}
		otl_Lookup *lookup = table->lookups[j];
		uint16_t lookupType =
		    useExtended
		        ? (lookup->type > otl_type_gpos_unknown
		               ? otl_type_gpos_extend - otl_type_gpos_unknown
		               : lookup->type > otl_type_gsub_unknown ? otl_type_gsub_extend - otl_type_gsub_unknown : 0)
		        : (lookup->type > otl_type_gpos_unknown
		               ? lookup->type - otl_type_gpos_unknown
		               : lookup->type > otl_type_gsub_unknown ? lookup->type - otl_type_gsub_unknown : 0);

		bk_Block *blk = bk_new_Block(b16, lookupType,            // LookupType
		                             b16, lookup->flags,         // LookupFlag
		                             b16, lookup->subtableCount, // SubTableCount
		                             bkover);
		for (tableid_t k = 0; k < lookup->subtableCount; k++) {
			if (useExtended) {
				uint16_t extensionLookupType =
				    lookup->type > otl_type_gpos_unknown
				        ? lookup->type - otl_type_gpos_unknown
				        : lookup->type > otl_type_gsub_unknown ? lookup->type - otl_type_gsub_unknown : 0;

				bk_Block *stub = bk_new_Block(b16, 1,                                      // format
				                              b16, extensionLookupType,                    // ExtensionLookupType
				                              p32, bk_newBlockFromBuffer(subtables[j][k]), // ExtensionOffset
				                              bkover);
				bk_push(blk, p16, stub, bkover);
			} else {
				bk_push(blk, p16, bk_newBlockFromBuffer(subtables[j][k]), bkover);
			}
		}
		bk_push(blk, b16, 0, // MarkFilteringSet
		        bkover);
		bk_push(root, p16, blk, bkover);
		free(subtables[j]);
	}
	free(subtables);
	free(lookupWritten);
	return root;
}

static uint32_t featureNameToTag(sds name) {
	uint32_t tag = 0;
	if (sdslen(name) > 0) { tag |= ((uint8_t)name[0]) << 24; }
	if (sdslen(name) > 1) { tag |= ((uint8_t)name[1]) << 16; }
	if (sdslen(name) > 2) { tag |= ((uint8_t)name[2]) << 8; }
	if (sdslen(name) > 3) { tag |= ((uint8_t)name[3]) << 0; }
	return tag;
}
static bk_Block *writeOTLFeatures(table_OTL *table, const caryll_Options *options) {
	bk_Block *root = bk_new_Block(b16, table->featureCount, bkover);
	for (tableid_t j = 0; j < table->featureCount; j++) {
		bk_Block *fea = bk_new_Block(p16, NULL,                            // FeatureParams
		                             b16, table->features[j]->lookupCount, // LookupCount
		                             bkover);
		for (tableid_t k = 0; k < table->features[j]->lookupCount; k++) {
			// reverse lookup
			for (tableid_t l = 0; l < table->lookupCount; l++) {
				if (table->features[j]->lookups[k] == table->lookups[l]) {
					bk_push(fea, b16, l, bkover);
					break;
				}
			}
		}
		bk_push(root, b32, featureNameToTag(table->features[j]->name), // FeatureTag
		        p16, fea,                                              // Feature
		        bkover);
	}
	return root;
}

typedef struct {
	sds tag;
	uint16_t lc;
	otl_LanguageSystem *dl;
	otl_LanguageSystem **ll;
	UT_hash_handle hh;
} script_stat_hash;

static tableid_t featureIndex(otl_Feature *feature, table_OTL *table) {
	for (tableid_t j = 0; j < table->featureCount; j++)
		if (table->features[j] == feature) { return j; }
	return 0xFFFF;
}
static bk_Block *writeLanguage(otl_LanguageSystem *lang, table_OTL *table) {
	if (!lang) return NULL;
	bk_Block *root = bk_new_Block(p16, NULL,                                       // LookupOrder
	                              b16, featureIndex(lang->requiredFeature, table), // ReqFeatureIndex
	                              b16, lang->featureCount,                         // FeatureCount
	                              bkover);
	for (tableid_t k = 0; k < lang->featureCount; k++) {
		bk_push(root, b16, featureIndex(lang->features[k], table), bkover);
	}
	return root;
}

static bk_Block *writeScript(script_stat_hash *script, table_OTL *table) {
	bk_Block *root = bk_new_Block(p16, writeLanguage(script->dl, table), // DefaultLangSys
	                              b16, script->lc,                       // LangSysCount
	                              bkover);

	for (tableid_t j = 0; j < script->lc; j++) {
		sds tag = sdsnewlen(script->ll[j]->name + 5, 4);

		bk_push(root, b32, featureNameToTag(tag),         // LangSysTag
		        p16, writeLanguage(script->ll[j], table), // LangSys
		        bkover);
	}
	return root;
}
static bk_Block *writeOTLScriptAndLanguages(table_OTL *table, const caryll_Options *options) {
	script_stat_hash *h = NULL;
	for (tableid_t j = 0; j < table->languageCount; j++) {
		sds scriptTag = sdsnewlen(table->languages[j]->name, 4);
		bool isDefault = strncmp(table->languages[j]->name + 5, "DFLT", 4) == 0 ||
		                 strncmp(table->languages[j]->name + 5, "dflt", 4) == 0;
		script_stat_hash *s = NULL;
		HASH_FIND_STR(h, scriptTag, s);
		if (s) {
			if (isDefault) {
				s->dl = table->languages[j];
			} else {
				s->lc += 1;
				s->ll[s->lc - 1] = table->languages[j];
			}
			sdsfree(scriptTag);
		} else {
			NEW(s);
			s->tag = scriptTag;
			s->dl = NULL;
			NEW_N(s->ll, table->languageCount);
			if (isDefault) {
				s->dl = table->languages[j];
				s->lc = 0;
			} else {
				s->lc = 1;
				s->ll[s->lc - 1] = table->languages[j];
			}
			HASH_ADD_STR(h, tag, s);
		}
	}

	bk_Block *root = bk_new_Block(b16, HASH_COUNT(h), bkover);

	script_stat_hash *s, *tmp;
	HASH_ITER(hh, h, s, tmp) {
		bk_push(root, b32, featureNameToTag(s->tag), // ScriptTag
		        p16, writeScript(s, table),          // Script
		        bkover);
		HASH_DEL(h, s);
		sdsfree(s->tag);
		free(s->ll);
		free(s);
	}
	return root;
}

caryll_buffer *table_build_otl(table_OTL *table, const caryll_Options *options, const char *tag) {
	bk_Block *root = bk_new_Block(b32, 0x10000,                                    // Version
	                              p16, writeOTLScriptAndLanguages(table, options), // ScriptList
	                              p16, writeOTLFeatures(table, options),           // FeatureList
	                              p16, writeOTLLookups(table, options, tag),       // LookupList
	                              bkover);
	return bk_build_Block(root);
}

////////////////////////////////////////////////////////////////////////////////////////
//                                    CONFIG PART //
////////////////////////////////////////////////////////////////////////////////////////

#define DELETE_TYPE(type, fn)                                                                                          \
	case type:                                                                                                         \
		fn(lookup->subtables[j]);                                                                                      \
		break;
#define LOOKUP_READER(llt, fn)                                                                                         \
	case llt:                                                                                                          \
		return fn(data, tableLength, subtableOffset);
#define LOOKUP_DUMPER(llt, fn) _declare_lookup_dumper(llt, tableNames[llt], fn, lookup, dump);
#define LOOKUP_PARSER(llt, parser)                                                                                     \
	if (!parsed) { parsed = _declareLookupParser(tableNames[llt], llt, parser, lookup, lookupName, lh); }
#define LOOKUP_WRITER(type, fn)                                                                                        \
	if (!written) written = _declare_lookup_writer(type, fn, lookup, subtables, lastOffset);
static const char *tableNames[] = {[otl_type_unknown] = "unknown",
                                   [otl_type_gsub_unknown] = "gsub_unknown",
                                   [otl_type_gsub_single] = "gsub_single",
                                   [otl_type_gsub_multiple] = "gsub_multiple",
                                   [otl_type_gsub_alternate] = "gsub_alternate",
                                   [otl_type_gsub_ligature] = "gsub_ligature",
                                   [otl_type_gsub_context] = "gsub_context",
                                   [otl_type_gsub_chaining] = "gsub_chaining",
                                   [otl_type_gsub_extend] = "gsub_extend",
                                   [otl_type_gsub_reverse] = "gsub_reverse",
                                   [otl_type_gpos_unknown] = "gpos_unknown",
                                   [otl_type_gpos_single] = "gpos_single",
                                   [otl_type_gpos_pair] = "gpos_pair",
                                   [otl_type_gpos_cursive] = "gpos_cursive",
                                   [otl_type_gpos_markToBase] = "gpos_mark_to_base",
                                   [otl_type_gpos_markToLigature] = "gpos_mark_to_ligature",
                                   [otl_type_gpos_markToMark] = "gpos_mark_to_mark",
                                   [otl_type_gpos_context] = "gpos_context",
                                   [otl_type_gpos_chaining] = "gpos_chaining",
                                   [otl_type_gpos_extend] = "gpos_extend"};

void caryll_delete_lookup(otl_Lookup *lookup) {
	if (!lookup) return;
	if (lookup->subtables) {
		for (tableid_t j = 0; j < lookup->subtableCount; j++) {
			switch (lookup->type) {
				DELETE_TYPE(otl_type_gsub_single, otl_delete_gsub_single);
				DELETE_TYPE(otl_type_gsub_multiple, otl_delete_gsub_multi);
				DELETE_TYPE(otl_type_gsub_alternate, otl_delete_gsub_multi);
				DELETE_TYPE(otl_type_gsub_ligature, otl_delete_gsub_ligature);
				DELETE_TYPE(otl_type_gsub_chaining, otl_delete_chaining);
				DELETE_TYPE(otl_type_gsub_reverse, otl_delete_gsub_reverse);
				DELETE_TYPE(otl_type_gpos_single, otl_delete_gpos_single);
				DELETE_TYPE(otl_type_gpos_pair, otl_delete_gpos_pair);
				DELETE_TYPE(otl_type_gpos_cursive, otl_delete_gpos_cursive);
				DELETE_TYPE(otl_type_gpos_chaining, otl_delete_chaining);
				DELETE_TYPE(otl_type_gpos_markToBase, otl_delete_gpos_markToSingle);
				DELETE_TYPE(otl_type_gpos_markToMark, otl_delete_gpos_markToSingle);
				DELETE_TYPE(otl_type_gpos_markToLigature, otl_delete_gpos_markToLigature);
				default:;
			}
		}
		free(lookup->subtables);
		sdsfree(lookup->name);
	}
	free(lookup);
}

otl_Subtable *table_read_otl_subtable(font_file_pointer data, uint32_t tableLength, uint32_t subtableOffset,
                                      otl_LookupType lookupType) {
	switch (lookupType) {
		LOOKUP_READER(otl_type_gsub_single, otl_read_gsub_single);
		LOOKUP_READER(otl_type_gsub_multiple, otl_read_gsub_multi);
		LOOKUP_READER(otl_type_gsub_alternate, otl_read_gsub_multi);
		LOOKUP_READER(otl_type_gsub_ligature, otl_read_gsub_ligature);
		LOOKUP_READER(otl_type_gsub_chaining, otl_read_chaining);
		LOOKUP_READER(otl_type_gsub_reverse, otl_read_gsub_reverse);
		LOOKUP_READER(otl_type_gpos_chaining, otl_read_chaining);
		LOOKUP_READER(otl_type_gsub_context, otl_read_contextual);
		LOOKUP_READER(otl_type_gpos_context, otl_read_contextual);
		LOOKUP_READER(otl_type_gpos_single, otl_read_gpos_single);
		LOOKUP_READER(otl_type_gpos_pair, otl_read_gpos_pair);
		LOOKUP_READER(otl_type_gpos_cursive, otl_read_gpos_cursive);
		LOOKUP_READER(otl_type_gpos_markToBase, otl_read_gpos_markToSingle);
		LOOKUP_READER(otl_type_gpos_markToMark, otl_read_gpos_markToSingle);
		LOOKUP_READER(otl_type_gpos_markToLigature, otl_read_gpos_markToLigature);
		LOOKUP_READER(otl_type_gsub_extend, table_read_otl_gsub_extend);
		LOOKUP_READER(otl_type_gpos_extend, table_read_otl_gpos_extend);
		default:
			return NULL;
	}
}

static void _dump_lookup(otl_Lookup *lookup, json_value *dump) {
	LOOKUP_DUMPER(otl_type_gsub_single, otl_gsub_dump_single);
	LOOKUP_DUMPER(otl_type_gsub_multiple, otl_gsub_dump_multi);
	LOOKUP_DUMPER(otl_type_gsub_alternate, otl_gsub_dump_multi);
	LOOKUP_DUMPER(otl_type_gsub_ligature, otl_gsub_dump_ligature);
	LOOKUP_DUMPER(otl_type_gsub_chaining, otl_dump_chaining);
	LOOKUP_DUMPER(otl_type_gsub_reverse, otl_gsub_dump_reverse);
	LOOKUP_DUMPER(otl_type_gpos_chaining, otl_dump_chaining);
	LOOKUP_DUMPER(otl_type_gpos_single, otl_gpos_dump_single);
	LOOKUP_DUMPER(otl_type_gpos_pair, otl_gpos_dump_pair);
	LOOKUP_DUMPER(otl_type_gpos_cursive, otl_gpos_dump_cursive);
	LOOKUP_DUMPER(otl_type_gpos_markToBase, otl_gpos_dump_markToSingle);
	LOOKUP_DUMPER(otl_type_gpos_markToMark, otl_gpos_dump_markToSingle);
	LOOKUP_DUMPER(otl_type_gpos_markToLigature, otl_gpos_dump_markToLigature);
}

static bool _parse_lookup(json_value *lookup, char *lookupName, lookup_hash **lh) {
	bool parsed = false;
	LOOKUP_PARSER(otl_type_gsub_single, otl_gsub_parse_single);
	LOOKUP_PARSER(otl_type_gsub_multiple, otl_gsub_parse_multi);
	LOOKUP_PARSER(otl_type_gsub_alternate, otl_gsub_parse_multi);
	LOOKUP_PARSER(otl_type_gsub_ligature, otl_gsub_parse_ligature);
	LOOKUP_PARSER(otl_type_gsub_chaining, otl_parse_chaining);
	LOOKUP_PARSER(otl_type_gsub_reverse, otl_gsub_parse_reverse);
	LOOKUP_PARSER(otl_type_gpos_single, otl_gpos_parse_single);
	LOOKUP_PARSER(otl_type_gpos_pair, otl_gpos_parse_pair);
	LOOKUP_PARSER(otl_type_gpos_cursive, otl_gpos_parse_cursive);
	LOOKUP_PARSER(otl_type_gpos_chaining, otl_parse_chaining);
	LOOKUP_PARSER(otl_type_gpos_markToBase, otl_gpos_parse_markToSingle);
	LOOKUP_PARSER(otl_type_gpos_markToMark, otl_gpos_parse_markToSingle);
	LOOKUP_PARSER(otl_type_gpos_markToLigature, otl_gpos_parse_markToLigature);
	return parsed;
}

static bool _build_subtable(otl_Lookup *lookup, caryll_buffer ***subtables, size_t *lastOffset) {
	bool written = false;
	LOOKUP_WRITER(otl_type_gsub_single, caryll_build_gsub_single_subtable);
	LOOKUP_WRITER(otl_type_gsub_multiple, caryll_build_gsub_multi_subtable);
	LOOKUP_WRITER(otl_type_gsub_alternate, caryll_build_gsub_multi_subtable);
	LOOKUP_WRITER(otl_type_gsub_ligature, caryll_build_gsub_ligature_subtable);
	LOOKUP_WRITER(otl_type_gsub_chaining, caryll_build_chaining);
	LOOKUP_WRITER(otl_type_gsub_reverse, caryll_build_gsub_reverse);
	LOOKUP_WRITER(otl_type_gpos_single, caryll_build_gpos_single);
	LOOKUP_WRITER(otl_type_gpos_pair, caryll_build_gpos_pair);
	LOOKUP_WRITER(otl_type_gpos_cursive, caryll_build_gpos_cursive);
	LOOKUP_WRITER(otl_type_gpos_chaining, caryll_build_chaining);
	LOOKUP_WRITER(otl_type_gpos_markToBase, caryll_build_gpos_markToSingle);
	LOOKUP_WRITER(otl_type_gpos_markToMark, caryll_build_gpos_markToSingle);
	LOOKUP_WRITER(otl_type_gpos_markToLigature, caryll_build_gpos_markToLigature);
	return written;
}
