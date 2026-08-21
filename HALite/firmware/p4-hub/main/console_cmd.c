#include "console_cmd.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_console.h"
#include "hub_cmd.h"
#include "hub_json.h"
#include "ipc_link.h"
#include "registry.h"

static int cmd_entities(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int n = registry_count();
    printf("entities %d  ipc=%s  permit=%s\n", n, registry_ipc_ok() ? "up" : "down",
           registry_permit_join() ? "on" : "off");
    for (int i = 0; i < n; i++) {
        const halite_entity_t *e = registry_get(i);
        if (!e) {
            continue;
        }
        printf("  %s  %s  %s\n", e->entity_id, e->name, e->on ? "on" : "off");
    }
    return 0;
}

static int cmd_permit(int argc, char **argv)
{
    bool on = true;
    if (argc > 1 && (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0)) {
        on = false;
    }
    esp_err_t err = ipc_link_permit_join(on, 0);
    printf("permit_join %s -> %s\n", on ? "on" : "off", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_toggle(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: toggle <entity_id>\n");
        return 1;
    }
    esp_err_t err = hub_command(argv[1], "toggle");
    printf("toggle %s -> %s\n", argv[1], esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_ping(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = ipc_link_ping();
    printf("ping -> %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_json(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0 || strcmp(sub, "entities") == 0) {
        hub_json_print_line(hub_json_status());
        return 0;
    }
    if (strcmp(sub, "ping") == 0) {
        esp_err_t err = ipc_link_ping();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
        cJSON_AddStringToObject(o, "err", esp_err_to_name(err));
        cJSON_AddBoolToObject(o, "ipc_c6", registry_ipc_ok());
        hub_json_print_line(o);
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "permit") == 0) {
        bool on = true;
        if (argc > 2 && (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "0") == 0)) {
            on = false;
        }
        esp_err_t err = ipc_link_permit_join(on, 0);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
        cJSON_AddStringToObject(o, "err", esp_err_to_name(err));
        cJSON_AddBoolToObject(o, "permit_join", on);
        hub_json_print_line(o);
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "toggle") == 0) {
        if (argc < 3) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", false);
            cJSON_AddStringToObject(o, "err", "usage: json toggle <entity_id>");
            hub_json_print_line(o);
            return 1;
        }
        esp_err_t err = hub_command(argv[2], "toggle");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
        cJSON_AddStringToObject(o, "err", esp_err_to_name(err));
        const halite_entity_t *e = registry_find(argv[2]);
        if (e) {
            cJSON_AddItemToObject(o, "entity", hub_json_entity(e));
        }
        hub_json_print_line(o);
        return err == ESP_OK ? 0 : 1;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "err", "unknown json subcommand");
    hub_json_print_line(o);
    return 1;
}

void console_cmd_register(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "entities", .help = "List entities", .func = cmd_entities},
        {.command = "permit", .help = "permit [on|off]", .func = cmd_permit},
        {.command = "toggle", .help = "toggle <entity_id>", .func = cmd_toggle},
        {.command = "ping", .help = "PING C6 over IPC", .func = cmd_ping},
        {.command = "json", .help = "json [status|permit on|off|toggle <id>|ping]", .func = cmd_json},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}
