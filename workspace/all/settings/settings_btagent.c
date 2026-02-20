#ifdef HAS_BTAGENT

#include <stdio.h>
#include <string.h>
#include <gio/gio.h>
#include <glib.h>

#include "settings_btagent.h"

#define AGENT_PATH "/com/nextui/agent"

static const gchar* agent_xml =
	"<node>"
	" <interface name=\"org.bluez.Agent1\">"
	"  <method name=\"Release\"/>"
	"  <method name=\"RequestPinCode\">"
	"   <arg type=\"o\" direction=\"in\"/>"
	"   <arg type=\"s\" direction=\"out\"/>"
	"  </method>"
	"  <method name=\"RequestPasskey\">"
	"   <arg type=\"o\" direction=\"in\"/>"
	"   <arg type=\"u\" direction=\"out\"/>"
	"  </method>"
	"  <method name=\"RequestConfirmation\">"
	"   <arg type=\"o\" direction=\"in\"/>"
	"   <arg type=\"u\" direction=\"in\"/>"
	"  </method>"
	"  <method name=\"RequestAuthorization\">"
	"   <arg type=\"o\" direction=\"in\"/>"
	"  </method>"
	"  <method name=\"Cancel\"/>"
	" </interface>"
	"</node>";

// Agent state
static GDBusConnection* agent_bus = NULL;
static GDBusNodeInfo* agent_introspection = NULL;
static guint agent_reg_id = 0;
static guint agent_signal_id = 0;
static int agent_registered = 0;

// Forward declarations
static void agent_method_call(GDBusConnection* connection,
							  const gchar* sender,
							  const gchar* object_path,
							  const gchar* interface_name,
							  const gchar* method_name,
							  GVariant* parameters,
							  GDBusMethodInvocation* invocation,
							  gpointer user_data);

static void properties_changed(GDBusConnection* connection,
							   const gchar* sender_name,
							   const gchar* object_path,
							   const gchar* interface_name,
							   const gchar* signal_name,
							   GVariant* params,
							   gpointer user_data);

static void set_adapter_pairable(int on);

// Idle callback to safely stop the agent outside the signal handler
static gboolean btagent_stop_idle(gpointer user_data) {
	(void)user_data;
	btagent_stop();
	return G_SOURCE_REMOVE;
}

// ============================================
// Agent method handler
// ============================================

static void agent_method_call(GDBusConnection* connection,
							  const gchar* sender,
							  const gchar* object_path,
							  const gchar* interface_name,
							  const gchar* method_name,
							  GVariant* parameters,
							  GDBusMethodInvocation* invocation,
							  gpointer user_data) {
	(void)connection;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)parameters;
	(void)user_data;

	if (strcmp(method_name, "RequestPinCode") == 0) {
		printf("BT Agent: RequestPinCode called\n");
		g_dbus_method_invocation_return_value(
			invocation, g_variant_new("(s)", "0000"));
		return;
	}

	if (strcmp(method_name, "RequestPasskey") == 0) {
		printf("BT Agent: RequestPasskey called\n");
		g_dbus_method_invocation_return_value(
			invocation, g_variant_new("(u)", 0));
		return;
	}

	g_dbus_method_invocation_return_value(invocation, NULL);
}

// ============================================
// Properties changed signal handler
// ============================================

static void properties_changed(GDBusConnection* connection,
							   const gchar* sender_name,
							   const gchar* object_path,
							   const gchar* interface_name,
							   const gchar* signal_name,
							   GVariant* params,
							   gpointer user_data) {
	(void)connection;
	(void)sender_name;
	(void)interface_name;
	(void)signal_name;
	(void)user_data;

	const gchar* iface = NULL;
	GVariantIter* iter = NULL;
	GVariant* val = NULL;
	const gchar* key = NULL;

	/* params = (sa{sv}as) */
	g_variant_get(params, "(&sa{sv}as)", &iface, &iter, NULL);

	if (strcmp(iface, "org.bluez.Device1") != 0) {
		g_variant_iter_free(iter);
		return;
	}

	while (g_variant_iter_next(iter, "{sv}", &key, &val)) {
		if (strcmp(key, "Paired") == 0 && g_variant_get_boolean(val)) {
			printf("BT Agent: Device paired: %s\n", object_path);
			g_idle_add(btagent_stop_idle, NULL);
		}
		g_variant_unref(val);
	}

	g_variant_iter_free(iter);
}

// ============================================
// Set adapter pairable/discoverable
// ============================================

static void set_adapter_pairable(int on) {
	g_dbus_connection_call_sync(
		agent_bus, "org.bluez", "/org/bluez/hci0",
		"org.freedesktop.DBus.Properties", "Set",
		g_variant_new("(ssv)", "org.bluez.Adapter1",
					  "Discoverable", g_variant_new_boolean(on)),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

	g_dbus_connection_call_sync(
		agent_bus, "org.bluez", "/org/bluez/hci0",
		"org.freedesktop.DBus.Properties", "Set",
		g_variant_new("(ssv)", "org.bluez.Adapter1",
					  "Pairable", g_variant_new_boolean(on)),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
}

// ============================================
// Public API
// ============================================

void btagent_start(void) {
	if (agent_registered)
		return;

	if (!agent_bus) {
		agent_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
		if (!agent_bus) {
			fprintf(stderr, "BT Agent: Failed to get system bus\n");
			return;
		}
	}

	// Parse introspection
	agent_introspection = g_dbus_node_info_new_for_xml(agent_xml, NULL);
	if (!agent_introspection || !agent_introspection->interfaces[0]) {
		fprintf(stderr, "BT Agent: Failed to parse D-Bus introspection XML\n");
		return;
	}

	// Register object
	static const GDBusInterfaceVTable vtable = {
		agent_method_call, NULL, NULL};

	agent_reg_id = g_dbus_connection_register_object(
		agent_bus, AGENT_PATH,
		agent_introspection->interfaces[0],
		&vtable, NULL, NULL, NULL);

	// Register with BlueZ
	g_dbus_connection_call_sync(
		agent_bus, "org.bluez", "/org/bluez",
		"org.bluez.AgentManager1", "RegisterAgent",
		g_variant_new("(os)", AGENT_PATH, "NoInputNoOutput"),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

	// Request default agent
	g_dbus_connection_call_sync(
		agent_bus, "org.bluez", "/org/bluez",
		"org.bluez.AgentManager1", "RequestDefaultAgent",
		g_variant_new("(o)", AGENT_PATH),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

	// Subscribe to property changes
	agent_signal_id = g_dbus_connection_signal_subscribe(
		agent_bus, "org.bluez",
		"org.freedesktop.DBus.Properties", "PropertiesChanged",
		NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		properties_changed, NULL, NULL);

	// Enable pairable
	set_adapter_pairable(1);

	agent_registered = 1;
	printf("BT Agent: Pairing window opened\n");
}

void btagent_stop(void) {
	if (!agent_registered)
		return;

	// Unregister from BlueZ
	g_dbus_connection_call_sync(
		agent_bus, "org.bluez", "/org/bluez",
		"org.bluez.AgentManager1", "UnregisterAgent",
		g_variant_new("(o)", AGENT_PATH),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

	// Disable pairable
	set_adapter_pairable(0);

	// Unsubscribe and unregister
	g_dbus_connection_signal_unsubscribe(agent_bus, agent_signal_id);
	g_dbus_connection_unregister_object(agent_bus, agent_reg_id);

	if (agent_introspection) {
		g_dbus_node_info_unref(agent_introspection);
		agent_introspection = NULL;
	}

	agent_registered = 0;
	agent_reg_id = 0;
	agent_signal_id = 0;

	if (agent_bus) {
		g_object_unref(agent_bus);
		agent_bus = NULL;
	}

	printf("BT Agent: Pairing window closed\n");
}

#endif // HAS_BTAGENT
