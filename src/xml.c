/**
 *
 *
 * 2019-01-06 - jdj - updates for attribute parsing
 * 2019-01-21 - jdj - removed malloc, using calloc to clear out structs
 *
 * Copyright (c) 2012 ooxi/xml.c
 *     https://github.com/ooxi/xml.c
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from the
 * use of this software.
 * 
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 *  1. The origin of this software must not be misrepresented; you must not
 *     claim that you wrote the original software. If you use this software in a
 *     product, an acknowledgment in the product documentation would be
 *     appreciated but is not required.
 * 
 *  2. Altered source versions must be plainly marked as such, and must not be
 *     misrepresented as being the original software.
 *
 *  3. This notice may not be removed or altered from any source distribution.
 */
#ifdef XML_PARSER_VERBOSE
#include <alloca.h>
#endif

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#ifndef _WIN32
#include <strings.h>
#endif

#include "xml.h"

int _XML_INFO_ENABLED = 0;
int _XML_DEBUG_ENABLED = 0;
int _XML_TRACE_ENABLED = 0;

/**
 * [OPAQUE API]
 *
 * UTF-8 text
 */
typedef struct xml_string {
	uint8_t const* buffer;
	size_t length;
	struct xml_string *attributes;
	bool is_self_closing_tag;

} xml_string_t;


/**
 * [OPAQUE API]
 *
 * An xml_node will always contain a tag name and a 0-terminated list of
 * children. Moreover it may contain text content.
 */
typedef struct xml_node {
    struct xml_string* name;
    struct xml_string* content;
    struct xml_string* attributes;
    struct xml_node** children;
} xml_node_t;

/**
 * [OPAQUE API]
 *
 * An xml_document simply contains the root node and the underlying buffer
 */
typedef struct xml_document {
    struct {
        uint8_t* buffer;
        size_t length;
    } buffer;
    
    struct xml_node* root;
} xml_document_t;


/**
 * [PRIVATE]
 *
 * Frees the resources allocated by the string
 *
 * @warning `buffer` must _not_ be freed, since it is a reference to the
 *     document's buffer
 */
static void xml_string_free(struct xml_string* string) {
    if(string) {
        //make sure to clear attributes
        if(string->attributes) {
            xml_string_free(string->attributes);
            string->attributes = NULL;
        }
   
        free(string);
    }
}

/**
 * [PRIVATE]
 *
 * Frees the resources allocated by the node
 */
static void xml_node_free(struct xml_node* node) {
    xml_string_free(node->name);
    node->name = NULL;
    
    if(node->attributes) {
        xml_string_free(node->attributes);
        node->attributes = NULL;
    }
    
    if (node->content) {
        xml_string_free(node->content);
        node->content = NULL;
    }
    
    struct xml_node** it = node->children;
    while (*it) {
        xml_node_free(*it);
        ++it;
    }
    free(node->children);
    node->children = NULL;
    
    free(node);
}

void print_substring(const uint8_t *str, int skip, int tail)
{
	int max_len = strlen((const char*)str);
	for(int i=skip; i < max_len && i < tail; i++)
		_XML_TRACEA("%c", str[i]);
}

void _print_substring_unsafe(const uint8_t *str, int skip, int tail)
{
    for(int i=skip; i < tail; i++)
        _XML_TRACEA("%c", str[i]);
    
}

void dump_xml_string(struct xml_string *node) {

	_XML_TRACEF("dump_xml_string::xml_string: len: %zu, is_self_closing: %i, val: |", node->length, node->is_self_closing_tag);
    _print_substring_unsafe(node->buffer, 0, node->length);

	_XML_TRACEA("|");

	if(node->attributes && node->attributes->length) {
		_XML_TRACEA(", attributes len: %zu, val: ", node->attributes->length);
		_print_substring_unsafe(node->attributes->buffer, 0, node->attributes->length);
	}

	_XML_TRACEA("\n");
}

/**
 * [PRIVATE]
 *
 * Parser context
 */
struct xml_parser {
	uint8_t* buffer;
	size_t position;
	size_t length;
};

/**
 * [PRIVATE]
 *
 * Character offsets
 */
enum xml_parser_offset {
	NO_CHARACTER = -1,
	CURRENT_CHARACTER = 0,
	NEXT_CHARACTER = 1,
};

/**
 * [PRIVATE]
 *
* @return Number of elements in 0-terminated array
 */
static size_t get_zero_terminated_array_elements(struct xml_node** nodes) {
	size_t elements = 0;

	while (nodes[elements]) {
		++elements;
	}

	return elements;
}

/**
 * [PRIVATE]
 *
 * @warning No UTF conversions will be attempted
 *
 * @return true iff a == b
 */
static _Bool xml_string_equals(struct xml_string* a, struct xml_string* b) {

	_XML_FRNSC("%d:xml_string_equals: a.len: %zu, b.len: %zd, values:\n", __LINE__, a->length, b->length);
	if (a->length != b->length) {
		dump_xml_string(a);
		dump_xml_string(b);

		_XML_FRNSC("%d::xml_string_equals - returning false\n", __LINE__);

		return false;
	}

	size_t i = 0; for (; i < a->length; ++i) {
		if (a->buffer[i] != b->buffer[i]) {
			_XML_FRNSC(", (%c != %c)", a->buffer[i], b->buffer[i]);
			_XML_FRNSC("%d::xml_string_equals - returning false\n", __LINE__);

			return false;
		}
		_XML_FRNSC(", (%c == %c)", a->buffer[i], b->buffer[i]);

	}
	_XML_FRNSC("\n%d::xml_string_equals - returning true\n", __LINE__);
	return true;
}

bool xml_string_equals_ignore_case(xml_string_t *a, char* b) {

	//strncasecmp
	int b_strlen = strlen(b);

	if(a->length != b_strlen)
		return false;

 	uint8_t* a_str = calloc(xml_string_length(a) + 1, sizeof(uint8_t));
	xml_string_copy(a, a_str, xml_string_length(a));
	int res = (strncasecmp((const char*)a_str, b, b_strlen) == 0);

	free(a_str);

	return res;
}

/**
 dont free here, as  xml_string_free(xml_node_name_string);
 is still a reference to the full xml document
 **/
bool xml_node_equals_ignore_case(xml_node_t *a, char* b) {
    
    
    xml_string_t* xml_node_name_string = xml_node_name(a);
    bool res = false;
    
    //if we contain a namespace specifier, ignore for now...
    bool 	namespace_specifier_found = false;
	size_t 	namespace_offset = 0;

    for(int i=0; i < xml_node_name_string->length && !namespace_specifier_found; i++) {

        if(xml_node_name_string->buffer[i] == ':') {
        	//FIXME - use real namespace support when searching for a node name

            xml_node_name_string->buffer += i+1;
            xml_node_name_string->length -= i+1;

            namespace_offset = i;
            namespace_specifier_found = true;
            break;
        }
    }
    res = xml_string_equals_ignore_case(xml_node_name_string, b);
    
    if(namespace_specifier_found) {
    	//reset
    	xml_node_name_string->buffer -= namespace_offset+1;
    	xml_node_name_string->length += namespace_offset+1;
    }

    return res;
}


/**
 * moving to public
 */
uint8_t* xml_string_clone(xml_string_t* s) {
	if (!s) {
		return 0;
	}
	uint8_t* clone = calloc(s->length + 1, sizeof(uint8_t));

	xml_string_copy(s, clone, s->length);
	clone[s->length] = 0;

	return clone;
}



uint8_t* xml_attributes_clone(xml_string_t* s) {
	if (!s) {
		return 0;
	}

	uint8_t* clone = calloc(s->attributes->length + 1, sizeof(uint8_t));

	xml_string_copy(s->attributes, clone, s->attributes->length);
	clone[s->attributes->length] = 0;

	return clone;
}

uint8_t* xml_attributes_clone_node(xml_node_t* node) {
    xml_string_t* xml_string = xml_node_name(node);
    uint8_t* xml_attributes_clone_ret = xml_attributes_clone(xml_string);
    
    //xml_string is a ptr reference, we can't free it here...
    //free(xml_string);
    
    return xml_attributes_clone_ret;
}

/**
 * [PRIVATE]
 *
 * Echos the parsers call stack for debugging purposes
 */
//#define XML_PARSER_VERBOSE

#ifdef XML_PARSER_VERBOSE
static void xml_parser_info(struct xml_parser* parser, char const* message) {
	_XML_FRNSC("xml_parser_info: %s", message);
}
#else
#define xml_parser_info(parser, message) {}
#endif



/**
 * [PRIVATE]
 *
 * Echos an error regarding the parser's source to the console
 */
static void xml_parser_error(struct xml_parser* parser, enum xml_parser_offset offset, char const* message) {
	int row = 0;
	int column = 0;


	size_t character = __MAX(0, __MIN(parser->length, parser->position + offset));

	size_t position = 0; for (; position < character; ++position) {
		column++;

		if ('\n' == parser->buffer[position]) {
			row++;
			column = 0;
		}
	}

	if (NO_CHARACTER != offset) {
	  //fprintf(stderr,	"xml_parser_error at %i:%i (is %c): %s\n",
	  //			row + 1, column, parser->buffer[character], message);
	} else {
  //	fprintf(stderr,	"xml_parser_error at %i:%i: %s\n",
  //				row + 1, column, message);
	}
}



/**
 * [PRIVATE]
 *
 * Returns the n-th not-whitespace byte in parser and 0 if such a byte does not
 * exist
 */
static uint8_t xml_parser_peek(struct xml_parser* parser, size_t n) {
	size_t position = parser->position;

	while (position < parser->length) {
		if (!isspace(parser->buffer[position])) {
			if (n == 0) {
				return parser->buffer[position];
			} else {
				--n;
			}
		}

		position++;
	}

	return 0;
}


static uint8_t xml_parser_peek_any(struct xml_parser* parser, size_t n) {
	size_t position = parser->position + n;

	if(position < parser->length) {
		return parser->buffer[position];
	}
	return 0;
}



/**
 * [PRIVATE]
 *
 * Moves the parser's position n bytes. If the new position would be out of
 * bounds, it will be converted to the bounds itself
 */
static void xml_parser_consume(struct xml_parser* parser, size_t n) {

	/* Debug information
	 */
	#ifdef XML_PARSER_VERBOSE

	char* consumed = alloca((n + 1) * sizeof(char));
	memcpy(consumed, &parser->buffer[parser->position], __MIN(n, parser->length - parser->position));
	consumed[n] = 0;


	size_t message_buffer_length = 512;
	char* message_buffer = alloca(512 * sizeof(char));
	snprintf(message_buffer, message_buffer_length, "Consuming %li bytes \"%s\"", (long)n, consumed);
	message_buffer[message_buffer_length - 1] = 0;

	xml_parser_info(parser, message_buffer);
	#endif


	/* Move the position forward
	 */
	parser->position += n;

	/* Don't go too far
	 *
	 * @warning Valid because parser->length must be greater than 0
	 */
	if (parser->position >= parser->length) {
		parser->position = parser->length - 1;
	}
}



/**
 * [PRIVATE]
 * 
 * Skips to the next non-whitespace character
 */
static void xml_skip_whitespace(struct xml_parser* parser) {
	xml_parser_info(parser, "whitespace");

	while (isspace(parser->buffer[parser->position])) {
		if (parser->position + 1 >= parser->length) {
			return;
		} else {
			parser->position++;
		}
	}
}



/**
 * [PRIVATE]
 *
 * Parses the name out of the an XML tag's ending
 *
 * ---( Example )---
 * tag_name>
 * ---
 */
static struct xml_string* xml_parse_tag_end(struct xml_parser* parser) {

	_XML_FRNSC("xml_parse_tag_end::enter, parser is at: %c, n: %zu\n",parser->buffer[parser->position], parser->position);

	xml_parser_info(parser, "tag_end");
	size_t start_name_start = parser->position;;
	size_t start_name_length = 0;
	int attribute_start = -1;
	size_t attribute_len = 0;
	size_t length=0;
	uint8_t last_char = 0;
	uint8_t current = 0;
	/* Parse until `>' or a whitespace is reached
	 */
	while (start_name_start + length < parser->length) {
		current = xml_parser_peek_any(parser, CURRENT_CHARACTER);
	//	printf("xml_parse_tag_end::checking %c, hex is: 0x%x, isspace: %d\n", current, current, isspace(current));

		 if (('>' == current)) { //end of tag
					break;
		 } else if(!isspace(current) && attribute_start == -1) {
			//read as tag name
			start_name_length++;
		} else if(isspace(current) && length && (attribute_start == -1)) {
			//start processing attributes if we have a space in the tag

			start_name_length = length;
			attribute_start = start_name_start + start_name_length + 1; //chomp our opening space
		} if(attribute_start != -1) {
			attribute_len = length - start_name_length;
		}
		xml_parser_consume(parser, 1);
		length++;
		last_char = current;
	}

	if(attribute_start == -1 && !attribute_len) {
		start_name_length = length;
	}
	_XML_FRNSC("xml_parse_tag_end::found '>' on start+length: %lu\n", start_name_start+length);


	/* Consume `>'
	 */
	if ('>' != xml_parser_peek(parser, CURRENT_CHARACTER)) {
		xml_parser_error(parser, CURRENT_CHARACTER, "xml_parse_tag_end::expected tag end");
		return 0;
	}
	xml_parser_consume(parser, 1);


	/* Return parsed tag name
	 */
	struct xml_string* name = calloc(1, sizeof(struct xml_string));
	name->attributes = calloc(1, sizeof(struct xml_string));

	if(start_name_start != -1) {
		name->buffer = &parser->buffer[start_name_start];
		name->length = start_name_length;
		name->is_self_closing_tag = last_char == '/';
		_XML_FRNSC("***** %d, len: %zu", attribute_start, attribute_len);
		if(attribute_start != -1 && attribute_len) {
			name->attributes->buffer = &parser->buffer[attribute_start];
			name->attributes->length = name->is_self_closing_tag ? attribute_len -1 : attribute_len;

		}
	}

	dump_xml_string(name);
	return name;
}



/**
 * [PRIVATE]
 *
 * Parses an opening XML tag without attributes
 *
 * ---( Example )---
 * <tag_name>
 * ---
 */
static struct xml_string* xml_parse_tag_open(struct xml_parser* parser) {
	xml_parser_info(parser, "tag_open");
	xml_skip_whitespace(parser);

	/* Consume `<'
	 */
	if ('<' != xml_parser_peek(parser, CURRENT_CHARACTER)) {
		xml_parser_error(parser, CURRENT_CHARACTER, "xml_parse_tag_open::expected opening tag");
		return 0;
	}
	xml_parser_consume(parser, 1);

    //remove optional ?, e.g. in <?
    if ('?' == xml_parser_peek(parser, CURRENT_CHARACTER)) {
        xml_parser_consume(parser, 1);
    }

	/* Consume tag name
	 */
	_XML_FRNSC("xml_parse_tag_open::before xml_parse_tag_end");
	return xml_parse_tag_end(parser);
}



/**
 * [PRIVATE]
 *
 * Parses an closing XML tag without attributes
 *
 * ---( Example )---
 * </tag_name>
 * ---
 */
static struct xml_string* xml_parse_tag_close(struct xml_parser* parser) {
	xml_parser_info(parser, "tag_close");
	xml_skip_whitespace(parser);

	/* Consume `</'
	 */
	if (('<' != xml_parser_peek(parser, CURRENT_CHARACTER))
		||	('/' != xml_parser_peek(parser, NEXT_CHARACTER))) {

		if ('<' != xml_parser_peek(parser, CURRENT_CHARACTER)) {
			xml_parser_error(parser, CURRENT_CHARACTER, "xml_parse_tag_close::expected closing tag `<'");
		}
		if ('/' != xml_parser_peek(parser, NEXT_CHARACTER)) {
			xml_parser_error(parser, NEXT_CHARACTER, "xml_parse_tag_close::expected closing tag `/'");
		}

		return 0;
	}
	xml_parser_consume(parser, 2);

	/* Consume tag name
	 */
	_XML_FRNSC("xml_parse_tag_close: 605");

	return xml_parse_tag_end(parser);
}



/**
 * [PRIVATE]
 *
 * Parses a tag's content
 *
 * ---( Example )---
 *     this is
 *   a
 *       tag {} content
 * ---
 *
 * @warning CDATA etc. is _not_ and will never be supported
 */
static struct xml_string* xml_parse_content(struct xml_parser* parser) {
	xml_parser_info(parser, "content");

	/* Whitespace will be ignored
	 */
	xml_skip_whitespace(parser);

	size_t start = parser->position;
	size_t length = 0;

	/* Consume until `<' is reached
	 */
	while (start + length < parser->length) {
		uint8_t current = xml_parser_peek(parser, CURRENT_CHARACTER);

		if ('<' == current) {
			break;
		} else {
			xml_parser_consume(parser, 1);
			length++;
		}
	}

	/* Next character must be an `<' or we have reached end of file
	 */
	if ('<' != xml_parser_peek(parser, CURRENT_CHARACTER)) {
		xml_parser_error(parser, CURRENT_CHARACTER, "xml_parse_content::expected <");
		return 0;
	}

	/* Ignore tailing whitespace
	 */
	while ((length > 0) && isspace(parser->buffer[start + length - 1])) {
		length--;
	}

	/* Return text
	 */
	struct xml_string* content = calloc(1, sizeof(struct xml_string));
	content->buffer = &parser->buffer[start];
	content->length = length;
	return content;
}



/**
 * [PRIVATE]
 * 
 * Parses an XML fragment node
 *
 * ---( Example without children )---
 * <Node>Text</Node>
 * ---
 *
 * ---( Example with children )---
 * <Parent>
 *     <Child>Text</Child>
 *     <Child>Text</Child>
 *     <Test>Content</Test>
 * </Parent>
 * ---
 */
static struct xml_node* xml_parse_node(struct xml_parser* parser) {
//	xml_parser_info(parser, "node");
	_XML_FRNSC("%d::xml_parse_node - enter\n", __LINE__);

	/* Setup variables
	 */
	struct xml_string* tag_open = 0;
	struct xml_string* tag_close = 0;
	struct xml_string* content = 0;

	struct xml_node** children = calloc(1, sizeof(struct xml_node*));
	children[0] = 0;


	/* Parse open tag
	 */
	_XML_FRNSC("xml_parse_node - before xml_parse_tag_open\n");

	tag_open = xml_parse_tag_open(parser);
	if (!tag_open) {
		xml_parser_error(parser, NO_CHARACTER, "xml_parse_node::tag_open");
		goto exit_failure;
	}

	/* If tag ends with `/' it's self closing, skip content lookup */
	if(tag_open->is_self_closing_tag) {
		goto node_creation;
	} else 	if (tag_open->length > 0 && '/' == tag_open->buffer[tag_open->length - 1]) {
		/* Drop `/'
		 */
		--tag_open->length;
		goto node_creation;
	}

	/* If the content does not start with '<', a text content is assumed
	 */
	if ('<' != xml_parser_peek(parser, CURRENT_CHARACTER)) {

		_XML_FRNSC("xml_parse_node - before xml_parse_content\n");

		content = xml_parse_content(parser);

		if (!content) {
			xml_parser_error(parser, 0, "xml_parse_node::content");
			goto exit_failure;
		}


	/* Otherwise children are to be expected
	 */
	} else while ('/' != xml_parser_peek(parser, NEXT_CHARACTER) && (parser->position < parser->length - 2)) {

		/* Parse child node
		 */
		_XML_FRNSC("xml_parse_node, calling recursive xml_parse_node, pos: %zu, len: %zd, repeat: %d\n", parser->position, parser->length, (parser->position < parser->length - 3));
		struct xml_node* child = xml_parse_node(parser);
		if (!child) {
			xml_parser_error(parser, NEXT_CHARACTER, "xml_parse_node::child");
			goto exit_failure;
		}

		/* Grow child array :)
		 */
		size_t old_elements = get_zero_terminated_array_elements(children);
		size_t new_elements = old_elements + 1;
		children = realloc(children, (new_elements + 1) * sizeof(struct xml_node*));

		/* Save child
		 */
		children[new_elements - 1] = child;
		children[new_elements] = 0;

		xml_skip_whitespace(parser);

	}


	/* Parse close tag
	 */
		if(parser->position < parser->length - 2) {
			_XML_FRNSC("xml_parse_node - before xml_parse_tag_close, pos: %zu, length: %zu", parser->position, parser->length);

		tag_close = xml_parse_tag_close(parser);
		if (!tag_close) {
			xml_parser_error(parser, NO_CHARACTER, "xml_parse_node::tag_close");
			goto exit_failure;
		}
	}


	/* Close tag has to match open tag
	 */
	_XML_FRNSC("xml_parse_node - before xml_string_equals: tag_open: %p, tag_closed: %p\n", tag_open, tag_close);

	if(tag_open && tag_close) {
		if (!xml_string_equals(tag_open, tag_close)) {
			xml_parser_error(parser, NO_CHARACTER, "xml_parse_node::tag missmatch");
			goto exit_failure;
		}
	}

	/* Return parsed node
	 */
	if(tag_close)
		xml_string_free(tag_close);

node_creation:;
	struct xml_node* node = calloc(1, sizeof(struct xml_node));
	node->name = tag_open;
	node->content = content;
	node->children = children;
	return node;


	/* A failure occured, so free all allocalted resources
	 */
exit_failure:
	if (tag_open) {
		xml_string_free(tag_open);
	}
	if (tag_close) {
		xml_string_free(tag_close);
	}
	if (content) {
		xml_string_free(content);
	}

	struct xml_node** it = children;
	while (*it) {
		xml_node_free(*it);
		++it;
	}
	free(children);

	return 0;
}





/**
 * [PUBLIC API]
 */
struct xml_document* xml_parse_document(uint8_t* buffer, size_t length) {

	/* Initialize parser
	 */
	struct xml_parser parser = {
		.buffer = buffer,
		.position = 0,
		.length = length
	};

	/* An empty buffer can never contain a valid document
	 */
	if (!length) {
		xml_parser_error(&parser, NO_CHARACTER, "xml_parse_document::length equals zero");
		return 0;
	}

	/* Parse the root node
	 */
	_XML_FRNSC("%d::xml_parse_document - enter", __LINE__);

	struct xml_node* root = xml_parse_node(&parser);
	if (!root) {
		xml_parser_error(&parser, NO_CHARACTER, "xml_parse_document::parsing document failed");
		return 0;
	}

	/* Return parsed document
	 */
	struct xml_document* document = calloc(1, sizeof(struct xml_document));
	document->buffer.buffer = buffer;
	document->buffer.length = length;
	document->root = root;

	return document;
}



/**
 * [PUBLIC API]
 * jjustman-2020-03-26 - do NOT take ownership of FILE* source - removed fclose(source) as handing off ownership should be a **
 */
struct xml_document* xml_open_document(FILE* source) {

	/* Prepare buffer
	 */
	size_t document_length = 0;

	size_t const read_chunk = 512; // TODO 4096;
	size_t buffer_size = 512;	// TODO 4069

	uint8_t* buffer = NULL;

	bool single_fread_success = false;

	//fstat(source, &st); - reports wrong size?
	fseek(source, 0, SEEK_END);
    long source_fp_size = ftell(source);
	fseek(source, 0, SEEK_SET);

	if(source_fp_size) {
		buffer = calloc(source_fp_size, sizeof(uint8_t));
		size_t read = fread(buffer, source_fp_size, 1, source);
		if(!read) {
			_XML_ERROR("%d::xml_open_document - fread returned: %zu", __LINE__, read);
		} else {
			single_fread_success = true;
			document_length = source_fp_size;
		}
	}

	//fallback to incremental resizing
	if(!single_fread_success) {

		//fallback
		buffer = calloc(buffer_size, sizeof(uint8_t));

		/* Read hole file into buffer
		 */
		while (!feof(source)) {

			/*
			 * Reallocate buffer
			 */

			if (buffer_size - document_length < read_chunk) {
				buffer = realloc(buffer, buffer_size + 2 * read_chunk);
				buffer_size += 2 * read_chunk;
			}

			size_t read = fread(
				&buffer[document_length],
				sizeof(uint8_t), read_chunk,
				source
			);

			document_length += read;
		}
	}

	/* Try to parse buffer
	 */
	struct xml_document* document = xml_parse_document(buffer, document_length);

	if (!document) {
		free(buffer);
		return 0;
	}
	return document;
}



/**
 * [PUBLIC API]
 */
void xml_document_free(xml_document_t* document, bool free_buffer) {
	xml_node_free(document->root);
    document->root = NULL;

	if (free_buffer) {
		free(document->buffer.buffer);
		document->buffer.buffer = NULL;
	}
	free(document);
	document = NULL;
}



/**
 * [PUBLIC API]
 */
struct xml_node* xml_document_root(struct xml_document* document) {
	return document->root;
}



/**
 * [PUBLIC API]
 */
struct xml_string* xml_node_name(struct xml_node* node) {
	return node->name;
}



/**
 * [PUBLIC API]
 */
struct xml_string* xml_node_content(struct xml_node* node) {
	return node->content;
}



/**
 * [PUBLIC API]
 *
 * @warning O(n)
 */
size_t xml_node_children(struct xml_node* node) {
	return get_zero_terminated_array_elements(node->children);
}



/**
 * [PUBLIC API]
 */
struct xml_node* xml_node_child(struct xml_node* node, size_t child) {
	if (child >= xml_node_children(node)) {
		return 0;
	}

	return node->children[child];
}



/**
 * [PUBLIC API]
 */
struct xml_node* xml_easy_child(struct xml_node* node, uint8_t const* child_name, ...) {

	/* Find children, one by one
	 */
	struct xml_node* current = node;

	va_list arguments;
	va_start(arguments, child_name);


	/* Descent to current.child
	 */
	while (child_name) {

		/* Convert child_name to xml_string for easy comparison
		 */
		struct xml_string cn = {
			.buffer = child_name,
			.length = strlen((const char*)child_name),
			.attributes = NULL
		};

		/* Iterate through all children
		 */
		struct xml_node* next = 0;

		size_t i = 0; for (; i < xml_node_children(current); ++i) {
			struct xml_node* child = xml_node_child(current, i);

			if (xml_string_equals(xml_node_name(child), &cn)) {
				if (!next) {
					next = child;

				/* Two children with the same name
				 */
				} else {
					va_end(arguments);
					return 0;
				}
			}
		}

		/* No child with that name found
		 */
		if (!next) {
			va_end(arguments);
			return 0;
		}
		current = next;		
		
		/* Find name of next child
		 */
		child_name = va_arg(arguments, uint8_t const*);
	}
	va_end(arguments);


	/* Return current element
	 */
	return current;
}



/**
 * [PUBLIC API]
 */
uint8_t* xml_easy_name(struct xml_node* node) {
	if (!node) {
		return 0;
	}

	return xml_string_clone(xml_node_name(node));
}



/**
 * [PUBLIC API]
 */
uint8_t* xml_easy_content(struct xml_node* node) {
	if (!node) {
		return 0;
	}

	return xml_string_clone(xml_node_content(node));
}



/**
 * [PUBLIC API]
 */
size_t xml_string_length(struct xml_string* string) {
	if (!string) {
		return 0;
	}
	return string->length;
}



/**
 * [PUBLIC API]
 */
void xml_string_copy(struct xml_string* string, uint8_t* buffer, size_t length) {
	if (!string) {
		return;
	}


	length = __MIN(length, string->length);


	memcpy(buffer, string->buffer, length);
}

