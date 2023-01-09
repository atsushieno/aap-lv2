/*
 * 
 * aap-import-lv2-metadata: generates aap_metadata.xml from LV2 metadata
 * 
 */

#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <serd/serd.h>
#include <sord/sord.h>
#include <lilv/lilv.h>
#include "lv2/core/lv2.h"
#include "lv2/atom/atom.h"
#include "lv2/midi/midi.h"
#include "lv2/presets/presets.h"

#define RDF__A LILV_NS_RDF "type"

#define AAP_CORE_URL "urn:org.androidaudioplugin.core"
#define AAP_PORT_PROPERTIES_URL "urn:org.androidaudioplugin.port"

LilvNode
	*rdf_a_uri_node,
	*atom_port_uri_node,
	*atom_supports_uri_node,
	*midi_event_uri_node,
	*instrument_plugin_uri_node,
	*audio_port_uri_node,
	/* *control_port_uri_node, */
	*input_port_uri_node,
	*output_port_uri_node,
	*port_property_uri_node,
	*toggled_uri_node,
	*integer_uri_node,
	*presets_uri_node;


#define PORTCHECKER_SINGLE(_name_,_type_) inline bool _name_ (const LilvPlugin* plugin, const LilvPort* port) { return lilv_port_is_a (plugin, port, _type_); }
#define PORTCHECKER_AND(_name_,_cond1_,_cond2_) inline bool _name_ (const LilvPlugin* plugin, const LilvPort* port) { return _cond1_ (plugin, port) && _cond2_ (plugin, port); }

PORTCHECKER_SINGLE (IS_AUDIO_PORT, audio_port_uri_node)
PORTCHECKER_SINGLE (IS_INPUT_PORT, input_port_uri_node)
PORTCHECKER_SINGLE (IS_OUTPUT_PORT, output_port_uri_node)
PORTCHECKER_SINGLE (IS_ATOM_PORT, atom_port_uri_node)
PORTCHECKER_AND (IS_AUDIO_IN, IS_AUDIO_PORT, IS_INPUT_PORT)
PORTCHECKER_AND (IS_AUDIO_OUT, IS_AUDIO_PORT, IS_OUTPUT_PORT)
/*PORTCHECKER_AND (IS_CONTROL_IN, IS_CONTROL_PORT, IS_INPUT_PORT)
PORTCHECKER_AND (IS_CONTROL_OUT, IS_CONTROL_PORT, IS_OUTPUT_PORT)*/

bool is_plugin_instrument(const LilvPlugin* plugin);
char* escape_xml(const char* s);


LilvWorld *world;

char* stringpool[4096];
int stringpool_entry = 0;

int main(int argc, const char **argv)
{
	bool showHelp = argc < 3;
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
			showHelp = true;
	if (showHelp) {
		fprintf (stderr, "Usage: %s [lib-lv2-dir] [res-xml-dir]\n", argv[0]);
		return 1;
	}
	
	const char* lv2dirName = argc < 2 ?  "lv2" : argv[1];
	const char* xmldir = argc < 3 ? "res/xml" : argv[2];
	
	world = lilv_world_new();

	rdf_a_uri_node = lilv_new_uri(world, RDF__A);
    atom_port_uri_node = lilv_new_uri (world, LV2_CORE__AudioPort);
    atom_supports_uri_node = lilv_new_uri (world, LV2_ATOM__supports);
    midi_event_uri_node = lilv_new_uri (world, LV2_MIDI__MidiEvent);
    instrument_plugin_uri_node = lilv_new_uri(world, LV2_CORE__InstrumentPlugin);
    audio_port_uri_node = lilv_new_uri (world, LV2_CORE__AudioPort);
    /*control_port_uri_node = lilv_new_uri (world, LV2_CORE__ControlPort);*/
    input_port_uri_node = lilv_new_uri (world, LV2_CORE__InputPort);
    output_port_uri_node = lilv_new_uri (world, LV2_CORE__OutputPort);
	port_property_uri_node = lilv_new_uri (world, LV2_CORE__portProperty);
	toggled_uri_node = lilv_new_uri (world, LV2_CORE__toggled);
	integer_uri_node = lilv_new_uri (world, LV2_CORE__integer);
	presets_uri_node = lilv_new_uri(world, LV2_PRESETS__Preset);

	char* lv2realpath = realpath(lv2dirName, NULL);
	fprintf(stderr, "LV2 directory: %s\n", lv2realpath);
	DIR *lv2dir = opendir(lv2dirName);
	if (!lv2dir) {
		fprintf(stderr, "Directory %s cannot be opened.\n", lv2dirName);
		return 3;
	}
	
	dirent *ent;
	while ((ent = readdir(lv2dir)) != NULL) {
		if (!strcmp(ent->d_name, "."))
			continue;
		if (!strcmp(ent->d_name, ".."))
			continue;
		
		int ttllen = snprintf(NULL, 0, "%s/%s/manifest.ttl", lv2realpath, ent->d_name) + 1;
		char* ttlfile = (char*) malloc(ttllen);
		stringpool[stringpool_entry++] = ttlfile;
		sprintf(ttlfile, "%s/%s/manifest.ttl", lv2realpath, ent->d_name);
		struct stat st;
		if(stat(ttlfile, &st)) {
			fprintf(stderr, "%s is not found.\n", ttlfile);
			continue;
		}
		fprintf(stderr, "Loading from %s\n", ttlfile);
		auto filePathNode = lilv_new_file_uri(world, NULL, ttlfile);
		
		lilv_world_load_bundle(world, filePathNode);
		
		fprintf (stderr, "Loaded bundle. Dumping all plugins from there.\n");
	}
	closedir(lv2dir);

	fprintf(stderr, "all plugins in %s are loaded\n", lv2dirName);
	
	const LilvPlugins *plugins = lilv_world_get_all_plugins(world);
	
	char *xmlFilename = (char*) calloc(snprintf(NULL, 0, "%s/aap_metadata.xml", xmldir) + 1, 1);
	sprintf(xmlFilename, "%s/aap_metadata.xml", xmldir);
	fprintf(stderr, "Writing metadata file %s\n", xmlFilename);
	FILE *xmlFP = fopen(xmlFilename, "w");
	if (!xmlFP) {
		fprintf(stderr, "Failed to create XML output file: %s\n", xmlFilename);
		fprintf(stderr, "Error code: %d\n", errno);
		return 1;
	}
	
	fprintf(xmlFP, "<plugins xmlns=\"%s\" xmlns:pp=\"%s\">\n", AAP_CORE_URL, AAP_PORT_PROPERTIES_URL);

	int numPlugins = lilv_plugins_size(plugins);
	char **pluginLv2Dirs = (char **) calloc(sizeof(char*) * numPlugins + 1, 1);
	int numPluginDirEntries = 0;

	for (auto i = lilv_plugins_begin(plugins); !lilv_plugins_is_end(plugins, i); i = lilv_plugins_next(plugins, i)) {
		
		const LilvPlugin *plugin = lilv_plugins_get(plugins, i);
		const char *name = lilv_node_as_string(lilv_plugin_get_name(plugin));
		const LilvNode *author = lilv_plugin_get_author_name(plugin);
		const LilvNode *manufacturer = lilv_plugin_get_project(plugin);

		char *bundle_path = strdup(lilv_file_uri_parse(lilv_node_as_uri(lilv_plugin_get_bundle_uri(plugin)), NULL));
		if (!bundle_path) {
			fprintf(stderr, "Failed to retrieve the plugin bundle path: %s\n", bundle_path);
			continue;
		}
		char *plugin_lv2dir = bundle_path;
		*strrchr(bundle_path, '/') = 0; // "/foo/bar/lv2/some.lv2"{/manifest.tll -> stripped}
		plugin_lv2dir = strrchr(plugin_lv2dir, '/'); // "/some.lv2"
		if (!plugin_lv2dir) {
			fprintf(stderr, "The bundle path did not meet the plugin path premise (/some/path/to/lv2/some.lv2/manifest.ttl): %s\n", plugin_lv2dir);
			free(bundle_path);
			continue;
		}
		plugin_lv2dir++;
		plugin_lv2dir = strdup(plugin_lv2dir);
		free(bundle_path);

		fprintf(xmlFP, "  <plugin backend=\"LV2\" name=\"%s\" category=\"%s\" author=\"%s\" manufacturer=\"%s\" unique-id=\"lv2:%s\" library=\"libandroidaudioplugin-lv2.so\" entrypoint=\"GetAndroidAudioPluginFactoryLV2Bridge\" assets=\"/lv2/%s/\">\n",
			name,
			/* FIXME: this categorization is super hacky */
			is_plugin_instrument(plugin) ? "Instrument" : "Effect",
			author != NULL ? escape_xml(lilv_node_as_string(author)) : "",
			manufacturer != NULL ? escape_xml(lilv_node_as_string(manufacturer)) : "",
			escape_xml(lilv_node_as_uri(lilv_plugin_get_uri(plugin))),
			escape_xml(plugin_lv2dir)
			);

		LilvNodes* presets = lilv_plugin_get_related(plugin, presets_uri_node);
		if (lilv_nodes_size(presets) > 0) {
			fprintf(xmlFP, "    <extensions>\n");
			fprintf(xmlFP, "      <extension uri='urn://androidaudioplugin.org/extensions/presets/v1' />\n");
			fprintf(xmlFP, "    </extensions>\n");
		}

		fprintf(xmlFP, "    <parameters xmlns='urn://androidaudioplugin.org/extensions/parameters'>\n");
		for (uint32_t p = 0; p < lilv_plugin_get_num_ports(plugin); p++) {
			auto port = lilv_plugin_get_port_by_index(plugin, p);
			if (IS_AUDIO_PORT(plugin, port))
				continue;

			LilvNode *defNode{nullptr}, *minNode{nullptr}, *maxNode{nullptr}, *propertyTypeNode{nullptr};
			lilv_port_get_range(plugin, port, &defNode, &minNode, &maxNode);
			LilvNodes *portProps = lilv_port_get_properties(plugin, port);
			bool isInteger{false};
			bool isToggled{false};
			LILV_FOREACH(nodes, pp, portProps) {
				auto portProp = lilv_nodes_get(portProps, pp);
				if (lilv_node_equals(portProp, integer_uri_node))
					isInteger = true;
				if (lilv_node_equals(portProp, toggled_uri_node))
					isToggled = true;
			}
			char def[1024], min[1024], max[1024], type[1024];
			def[0] = 0;
			min[0] = 0;
			max[0] = 0;
			type[0] = 0;
			if (isToggled) {
				std::snprintf(type, 1024, "type=\"%s\"", "boolean");
				if (defNode != nullptr) std::snprintf(def, 1024, "default=\"%s\"", lilv_node_as_float(defNode) > 0.0 ? "1" : "0");
			} else if (isInteger) {
				std::snprintf(type, 1024, "type=\"%s\"", "integer");
				if (defNode != nullptr) std::snprintf(def, 1024, "default=\"%i\"", lilv_node_as_int(defNode));
				if (minNode != nullptr) std::snprintf(min, 1024, "minimum=\"%i\"", lilv_node_as_int(minNode));
				if (maxNode != nullptr) std::snprintf(max, 1024, "maximum=\"%i\"", lilv_node_as_int(maxNode));
			} else {
				type[0] = 0;
				if (defNode != nullptr) std::snprintf(def, 1024, "default=\"%f\"", lilv_node_as_float(defNode));
				if (minNode != nullptr) std::snprintf(min, 1024, "minimum=\"%f\"", lilv_node_as_float(minNode));
				if (maxNode != nullptr) std::snprintf(max, 1024, "maximum=\"%f\"", lilv_node_as_float(maxNode));
			}
			
			fprintf(xmlFP, "      <parameter id=\"%d\" name=\"%s\" %s %s %s %s",
				lilv_port_get_index(plugin, port),
				escape_xml(lilv_node_as_string(lilv_port_get_name(plugin, port))),
				def, min, max, type);
			LilvScalePoints* scalePoints = lilv_port_get_scale_points(plugin, port);
			if (scalePoints != nullptr) {
			    fprintf(xmlFP, ">\n");
				LILV_FOREACH(scale_points, spi, scalePoints) {
					auto sp = lilv_scale_points_get(scalePoints, spi);
					auto labelNode = lilv_scale_point_get_label(sp);
					auto valueNode = lilv_scale_point_get_value(sp);
					auto label = escape_xml(lilv_node_as_string(labelNode));
					auto value = escape_xml(lilv_node_as_string(valueNode));
					fprintf(xmlFP, "        <enumeration label=\"%s\" value=\"%s\" />\n", label, value);
				}
				fprintf(xmlFP, "      </parameter>\n");
				lilv_scale_points_free(scalePoints);
			} else if (isToggled) {
				// kind of hacky way to support boolean...
				fprintf(xmlFP, ">\n");
				fprintf(xmlFP, "        <enumeration label=\"true\" value=\"1\" />\n");
				fprintf(xmlFP, "        <enumeration label=\"false\" value=\"0\" />\n");
				fprintf(xmlFP, "      </parameter>\n");
			}
			else
			    fprintf(xmlFP, " />\n");

			if(defNode) lilv_node_free(defNode);
			if(minNode) lilv_node_free(minNode);
			if(maxNode) lilv_node_free(maxNode);
			if(propertyTypeNode) lilv_node_free(propertyTypeNode);
		}
		fprintf(xmlFP, "    </parameters>\n");

		fprintf(xmlFP, "    <ports>\n");
		for (uint32_t p = 0; p < lilv_plugin_get_num_ports(plugin); p++) {
			auto port = lilv_plugin_get_port_by_index(plugin, p);
			auto nameNode = lilv_port_get_name(plugin, port);
			if (IS_AUDIO_PORT(plugin, port))
				fprintf(xmlFP, "      <port direction='%s' content='audio' name='%s' />\n",
					IS_INPUT_PORT(plugin, port) ? "input" : "output",
					nameNode ? lilv_node_as_string(nameNode) : IS_INPUT_PORT(plugin, port) ? "(Audio In)" : "(Audio Out)");
			if (name)
				free(nameNode);
		}
		fprintf(xmlFP, "      <port direction='input' content='midi2' name='MIDI In' />\n");
		fprintf(xmlFP, "      <port direction='output' content='midi2' name='MIDI Out' />\n");
		fprintf(xmlFP, "    </ports>\n");

		fprintf(xmlFP, "  </plugin>\n");

		for(int p = 0; p < numPlugins; p++) {
			if(!pluginLv2Dirs[p]) {
				pluginLv2Dirs[p] = plugin_lv2dir;
				numPluginDirEntries++;
				break;
			}
			else if(!strcmp(pluginLv2Dirs[p], plugin_lv2dir))
				break;
		}
	}
	
	fprintf(xmlFP, "</plugins>\n");
	fclose(xmlFP);
	free(xmlFilename);
	
	lilv_world_free(world);
	
	for(int i = 0; i < stringpool_entry; i++)
		free(stringpool[i]);
	free(lv2realpath);
	
	fprintf (stderr, "done.\n");
	return 0;
}

bool is_plugin_instrument(const LilvPlugin* plugin)
{
	/* If the plugin is `a lv2:InstrumentPlugin` then true. */
	auto nodes = lilv_world_find_nodes(world, lilv_plugin_get_uri(plugin), rdf_a_uri_node, NULL);
	LILV_FOREACH(nodes, n, nodes) {
		auto node = lilv_nodes_get(nodes, n);
		if(lilv_node_equals(node, instrument_plugin_uri_node))
			return true;
	}
	return false;
}

char* escape_xml(const char* s)
{
	auto ret = strdup(s);
	for (int i = 0; ret[i]; i++)
		switch (ret[i]) {
		case '<':
		case '>':
		case '&':
		case '"':
		case '\'':
			ret[i] = '_';
			break;
		}
	stringpool[stringpool_entry++] = ret;
	return ret;
}
