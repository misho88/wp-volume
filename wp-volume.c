#include <wp/wp.h>
#include <stdio.h>
#include <math.h>
#include <locale.h>
#include <spa/utils/defs.h>
#include <pipewire/keys.h>
#include <pipewire/extensions/session-manager/keys.h>

#define APPNAME "wp-volume"

struct context
{
	WpCore * core;
	GMainLoop * loop;
	WpObjectManager * om;
	unsigned int pending_plugins;
	char device_spec;
	int volume_spec;
	int exit_code;
};

struct get_id_ret { guint32 id; int error; }
get_id(WpPlugin * api, int capture)
{
	struct get_id_ret ret = { 0 };
	g_signal_emit_by_name(api, "get-default-node", capture ? "Audio/Source" : "Audio/Sink", &ret.id);
	ret.error = ret.id == 0 || ret.id == G_MAXUINT32;
	return ret;
}


struct get_volume_ret { double volume; int mute; int error; }
get_volume(WpPlugin * api, unsigned int id)
{
	GVariant * variant = NULL;
	g_signal_emit_by_name(api, "get-volume", id, &variant);
	if (!variant) return (struct get_volume_ret){ .error = 1 };

	struct get_volume_ret ret = { 0 };
	g_variant_lookup(variant, "volume", "d", &ret.volume);
	g_variant_lookup(variant, "mute", "b", &ret.mute);
	g_clear_pointer(&variant, g_variant_unref);
	return ret;
}

int
set_mute(WpPlugin * api, unsigned int id, int mute)
{
	g_auto(GVariantBuilder) builder = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "mute", g_variant_new_boolean(mute));
	GVariant * variant = g_variant_builder_end(&builder);
	int rv;
	g_signal_emit_by_name(api, "set-volume", id, variant, &rv);
	return rv;
}

int
set_volume(WpPlugin * api, unsigned int id, double volume)
{
	g_auto(GVariantBuilder) builder = G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "volume", g_variant_new_double(volume));
	GVariant * variant = g_variant_builder_end(&builder);
	int rv;
	g_signal_emit_by_name(api, "set-volume", id, variant, &rv);
	return rv;
}


static void
async_quit(WpCore *, GAsyncResult *, struct context * context)
{
	g_main_loop_quit(context->loop);
}

static void
async_main(struct context * context)
{
	int rv;
	g_autoptr(GError) error = NULL;

	g_autoptr(WpPlugin) def_nodes_api = wp_plugin_find(context->core, "default-nodes-api");
	g_autoptr(WpPlugin) mixer_api = wp_plugin_find(context->core, "mixer-api");

	struct get_id_ret get_id_ret = get_id(def_nodes_api, context->device_spec == 'c');
	if (get_id_ret.error) { fprintf(stderr, "failed to get node ID"); goto error; };
	unsigned int id = get_id_ret.id;

	char * failed_feature = NULL;

	struct get_volume_ret volume = get_volume(mixer_api, id);
	if (volume.error) { failed_feature = "reading volume/mute"; goto error; }

	switch(context->volume_spec) {
	case 0x1000 + 'g':
		printf("%d %s%c",
			(int)round(volume.volume * 100),
			volume.mute ? "off" : "on",
			context->device_spec == 'b' ? ' ' : '\n'
		);
		break;
	case 0x1000 + 'm':
	case 0x1000 + 'u':
	case 0x1000 + 't':
		int mute = context->volume_spec == 0x1000 + 't'
				? !volume.mute
				: context->volume_spec == 0x1000 + 'm';
		rv = set_mute(mixer_api, id, mute);
		if (!rv) { failed_feature = "setting mute"; goto error; }
		break;
	default:
		double vol = context->volume_spec >= 0x2000
				? (context->volume_spec - 0x2000) / 100.0
				: volume.volume + context->volume_spec / 100.0;
		rv = set_volume(mixer_api, id, vol);
		if (!rv) { failed_feature = "setting volume"; goto error; }
		break;
	}
	if (context->device_spec == 'b')
		goto repeat_for_capture;
	else
		goto success;

repeat_for_capture:
	context->device_spec = 'c';
	async_main(context);
	return;
error:
	fprintf(
		stderr,
		APPNAME ": %s device (node %d) does not support %s\n",
		context->device_spec == 'c' ? "capture" : "playback",
		id,
		failed_feature
	);
	context->exit_code = 1;
	g_main_loop_quit(context->loop);
	return;
success:
	wp_core_sync(context->core, NULL, (GAsyncReadyCallback)async_quit, context);
	return;
}

static void
on_plugin_activated(WpObject * plugin, GAsyncResult * res, struct context * context)
{
	g_autoptr(GError) error = NULL;
	int rv;

	rv = wp_object_activate_finish(plugin, res, &error);
	if (!rv) {
		fprintf (stderr, "%s", error->message);
		context->exit_code = 1;
		g_main_loop_quit(context->loop);
		return;
	}

	if (--context->pending_plugins == 0)
		wp_core_install_object_manager(context->core, context->om);
}

int
sync_main(WpCore * core, GMainLoop * loop, char device_spec, int volume_spec)
{
	struct context context = {
		.core = core,
		.loop = loop,
		.om = wp_object_manager_new(),
		.device_spec = device_spec,
		.volume_spec = volume_spec
	};

	g_autoptr(WpPlugin) def_nodes_api = wp_plugin_find(context.core, "default-nodes-api");
	g_autoptr(WpPlugin) mixer_api = wp_plugin_find(context.core, "mixer-api");
	g_object_set(G_OBJECT(mixer_api), "scale", 1 /* cubic */, NULL);

	WpPlugin * plugins[] = { def_nodes_api, mixer_api };
	for (unsigned int i = 0; i < sizeof(plugins) / sizeof(plugins[0]); i++) {
		WpPlugin * plugin = plugins[i];
		context.pending_plugins++;
		wp_object_activate(
			WP_OBJECT(plugin),
			WP_PLUGIN_FEATURE_ENABLED,
			NULL,
			(GAsyncReadyCallback)on_plugin_activated,
			&context
		);
	}

	int rv = wp_core_connect(context.core);
	if (!rv) { fprintf(stderr, "Could not connect to PipeWire\n"); return 2; }

	context.om = wp_object_manager_new();
	wp_object_manager_add_interest(context.om, WP_TYPE_ENDPOINT, NULL);
	wp_object_manager_add_interest(context.om, WP_TYPE_NODE, NULL);
	wp_object_manager_add_interest(context.om, WP_TYPE_CLIENT, NULL);
	wp_object_manager_request_object_features(context.om, WP_TYPE_GLOBAL_PROXY, WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);

	g_signal_connect_swapped(context.core, "disconnected", (GCallback)g_main_loop_quit, context.loop);
	g_signal_connect_swapped(context.om, "installed", (GCallback)async_main, &context);

	g_main_loop_run(context.loop);

	g_clear_object(&context.om);

	return context.exit_code;
}

#define PARSE_SPECS(OFFSET, ...) do { \
	char * specs[] = { __VA_ARGS__ }; \
	for (unsigned int i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) \
		if (strcmp(arg, specs[i]) == 0) \
			return (OFFSET) + specs[i][0]; \
} while (0)

char
get_device_spec(char * arg)
{
	PARSE_SPECS('\0', "p", "c", "b", "playback", "capture", "both");
	return '\0';
}

int
get_volume_spec(char * arg)
{
	if (*arg == '\0') return 0xBADBAD;

	PARSE_SPECS(0x1000, "g", "m", "u", "t", "get", "mute", "unmute", "toggle");
	char * endptr;
	long vol = strtol(arg, &endptr, 10);
	if (*endptr != '\0') return 0xBADBAD;

	if (*arg == '+' || *arg == '-') return (int)vol;
	return 0x2000 + (int)vol;
}
void
usage_exit(char * name)
{
	fprintf(stderr, "usage: %s [p[layback]|c[apture]|b[oth] [g[et]|m[ute]|u[nmute]|t[oggle]|[+|-]0-100]]\n", name);
	exit(1);
}

int
main(int argc, char ** argv)
{
	setlocale(LC_ALL, "");
	setlocale(LC_NUMERIC, "C");

	if (argc > 3) usage_exit(argv[0]);
	char device_spec = get_device_spec(argc > 1 ? argv[1] : "b");
	if (device_spec == '\0') usage_exit(argv[0]);
	int volume_spec = get_volume_spec(argc > 2 ? argv[2] : "g");
	if (volume_spec == 0xBADBAD) usage_exit(argv[0]);

	wp_init(WP_INIT_ALL);

	WpCore * core = wp_core_new(NULL, NULL);

	const char * components[] = { "libwireplumber-module-default-nodes-api", "libwireplumber-module-mixer-api" };
	for (unsigned int i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
		g_autoptr(GError) error = NULL;
		int rv = wp_core_load_component(core, components[i], "module", NULL, &error);
		if (!rv) { fprintf(stderr, "%s\n", error->message); return 1; }
	}

	int rv = 0;
	GMainLoop * loop = g_main_loop_new(NULL, FALSE);
	rv = sync_main(core, loop, device_spec, volume_spec);
	if (rv == 0 && volume_spec != 0x1000 + 'g')
		rv = sync_main(core, loop, device_spec, 0x1000 + 'g');
	g_clear_pointer(&loop, g_main_loop_unref);
	g_clear_object(&core);

	return rv;
}
