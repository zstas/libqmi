/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * qmicli -- Command line interface to control QMI devices
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2012 Aleksander Morgado <aleksander@gnu.org>
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include <libqmi-glib.h>

#include "qmicli.h"

/* Context */
typedef struct {
    QmiDevice *device;
    QmiClientNas *client;
    GCancellable *cancellable;
} Context;
static Context *ctx;

/* Options */
static gboolean get_signal_strength_flag;
static gboolean get_signal_info_flag;
static gboolean get_serving_system_flag;
static gboolean get_technology_preference_flag;
static gboolean get_system_selection_preference_flag;
static gboolean network_scan_flag;
static gboolean reset_flag;
static gboolean noop_flag;

static GOptionEntry entries[] = {
    { "nas-get-signal-strength", 0, 0, G_OPTION_ARG_NONE, &get_signal_strength_flag,
      "Get signal strength",
      NULL
    },
    { "nas-get-signal-info", 0, 0, G_OPTION_ARG_NONE, &get_signal_info_flag,
      "Get signal info",
      NULL
    },
    { "nas-get-serving-system", 0, 0, G_OPTION_ARG_NONE, &get_serving_system_flag,
      "Get serving system",
      NULL
    },
    { "nas-get-technology-preference", 0, 0, G_OPTION_ARG_NONE, &get_technology_preference_flag,
      "Get technology preference",
      NULL
    },
    { "nas-get-system-selection-preference", 0, 0, G_OPTION_ARG_NONE, &get_system_selection_preference_flag,
      "Get system selection preference",
      NULL
    },
    { "nas-network-scan", 0, 0, G_OPTION_ARG_NONE, &network_scan_flag,
      "Scan networks",
      NULL
    },
    { "nas-reset", 0, 0, G_OPTION_ARG_NONE, &reset_flag,
      "Reset the service state",
      NULL
    },
    { "nas-noop", 0, 0, G_OPTION_ARG_NONE, &noop_flag,
      "Just allocate or release a NAS client. Use with `--client-no-release-cid' and/or `--client-cid'",
      NULL
    },
    { NULL }
};

GOptionGroup *
qmicli_nas_get_option_group (void)
{
	GOptionGroup *group;

	group = g_option_group_new ("nas",
	                            "NAS options",
	                            "Show Network Access Service options",
	                            NULL,
	                            NULL);
	g_option_group_add_entries (group, entries);

	return group;
}

gboolean
qmicli_nas_options_enabled (void)
{
    static guint n_actions = 0;
    static gboolean checked = FALSE;

    if (checked)
        return !!n_actions;

    n_actions = (get_signal_strength_flag +
                 get_signal_info_flag +
                 get_serving_system_flag +
                 get_technology_preference_flag +
                 get_system_selection_preference_flag +
                 network_scan_flag +
                 reset_flag +
                 noop_flag);

    if (n_actions > 1) {
        g_printerr ("error: too many NAS actions requested\n");
        exit (EXIT_FAILURE);
    }

    checked = TRUE;
    return !!n_actions;
}

static void
context_free (Context *ctx)
{
    if (!ctx)
        return;

    if (ctx->cancellable)
        g_object_unref (ctx->cancellable);
    if (ctx->device)
        g_object_unref (ctx->device);
    if (ctx->client)
        g_object_unref (ctx->client);
    g_slice_free (Context, ctx);
}

static void
shutdown (gboolean operation_status)
{
    /* Cleanup context and finish async operation */
    context_free (ctx);
    qmicli_async_operation_done (operation_status);
}

static gdouble
get_db_from_sinr_level (QmiNasEvdoSinrLevel level)
{
    switch (level) {
    case QMI_NAS_EVDO_SINR_LEVEL_0: return -9.0;
    case QMI_NAS_EVDO_SINR_LEVEL_1: return -6;
    case QMI_NAS_EVDO_SINR_LEVEL_2: return -4.5;
    case QMI_NAS_EVDO_SINR_LEVEL_3: return -3;
    case QMI_NAS_EVDO_SINR_LEVEL_4: return -2;
    case QMI_NAS_EVDO_SINR_LEVEL_5: return 1;
    case QMI_NAS_EVDO_SINR_LEVEL_6: return 3;
    case QMI_NAS_EVDO_SINR_LEVEL_7: return 6;
    case QMI_NAS_EVDO_SINR_LEVEL_8: return +9;
    default:
        g_warning ("Invalid SINR level '%u'", level);
        return -G_MAXDOUBLE;
    }
}

static void
get_signal_info_ready (QmiClientNas *client,
                       GAsyncResult *res)
{
    QmiMessageNasGetSignalInfoOutput *output;
    GError *error = NULL;
    gint8 rssi;
    guint16 ecio;
    QmiNasEvdoSinrLevel sinr_level;
    gint32 io;
    gint8 rsrq;
    gint16 rsrp;
    gint16 snr;
    gint8 rscp;

    output = qmi_client_nas_get_signal_info_finish (client, res, &error);
    if (!output) {
        g_printerr ("error: operation failed: %s\n", error->message);
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_signal_info_output_get_result (output, &error)) {
        g_printerr ("error: couldn't get signal info: %s\n", error->message);
        g_error_free (error);
        qmi_message_nas_get_signal_info_output_unref (output);
        shutdown (FALSE);
        return;
    }

    g_print ("[%s] Successfully got signal info\n",
             qmi_device_get_path_display (ctx->device));

    /* CDMA... */
    if (qmi_message_nas_get_signal_info_output_get_cdma_signal_strength (output,
                                                                         &rssi,
                                                                         &ecio,
                                                                         NULL)) {
        g_print ("CDMA:\n"
                 "\tRSSI: '%d dBm'\n"
                 "\tECIO: '%.1lf dBm'\n",
                 rssi,
                 (-0.5)*((gdouble)ecio));
    }

    /* HDR... */
    if (qmi_message_nas_get_signal_info_output_get_hdr_signal_strength (output,
                                                                        &rssi,
                                                                        &ecio,
                                                                        &sinr_level,
                                                                        &io,
                                                                        NULL)) {
        g_print ("HDR:\n"
                 "\tRSSI: '%d dBm'\n"
                 "\tECIO: '%.1lf dBm'\n"
                 "\tSINR (%u): '%.1lf dB'\n"
                 "\tIO: '%d dBm'\n",
                 rssi,
                 (-0.5)*((gdouble)ecio),
                 sinr_level, get_db_from_sinr_level (sinr_level),
                 io);
    }

    /* GSM */
    if (qmi_message_nas_get_signal_info_output_get_gsm_signal_strength (output,
                                                                        &rssi,
                                                                        NULL)) {
        g_print ("GSM:\n"
                 "\tRSSI: '%d dBm'\n",
                 rssi);
    }

    /* WCDMA... */
    if (qmi_message_nas_get_signal_info_output_get_wcdma_signal_strength (output,
                                                                          &rssi,
                                                                          &ecio,
                                                                          NULL)) {
        g_print ("WCDMA:\n"
                 "\tRSSI: '%d dBm'\n"
                 "\tECIO: '%.1lf dBm'\n",
                 rssi,
                 (-0.5)*((gdouble)ecio));
    }

    /* LTE... */
    if (qmi_message_nas_get_signal_info_output_get_lte_signal_strength (output,
                                                                        &rssi,
                                                                        &rsrq,
                                                                        &rsrp,
                                                                        &snr,
                                                                        NULL)) {
        g_print ("LTE:\n"
                 "\tRSSI: '%d dBm'\n"
                 "\tRSRQ: '%d dB'\n"
                 "\tRSRP: '%d dBm'\n"
                 "\tSNR: '%.1lf dBm'\n",
                 rssi,
                 rsrq,
                 rsrp,
                 (0.1) * ((gdouble)snr));
    }

    /* TDMA */
    if (qmi_message_nas_get_signal_info_output_get_tdma_signal_strength (output,
                                                                         &rscp,
                                                                         NULL)) {
        g_print ("TDMA:\n"
                 "\tRSCP: '%d dBm'\n",
                 rscp);
    }

    qmi_message_nas_get_signal_info_output_unref (output);
    shutdown (TRUE);
}

static QmiMessageNasGetSignalStrengthInput *
get_signal_strength_input_create (void)
{
    GError *error = NULL;
    QmiMessageNasGetSignalStrengthInput *input;
    QmiNasSignalStrengthRequest mask;

    mask = (QMI_NAS_SIGNAL_STRENGTH_REQUEST_RSSI |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_ECIO |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_IO |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_SINR |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_RSRQ |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_LTE_SNR |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_LTE_RSRP);

    input = qmi_message_nas_get_signal_strength_input_new ();
    if (!qmi_message_nas_get_signal_strength_input_set_request_mask (
            input,
            mask,
            &error)) {
        g_printerr ("error: couldn't create input data bundle: '%s'\n",
                    error->message);
        g_error_free (error);
        qmi_message_nas_get_signal_strength_input_unref (input);
        input = NULL;
    }

    return input;
}

static void
get_signal_strength_ready (QmiClientNas *client,
                           GAsyncResult *res)
{
    QmiMessageNasGetSignalStrengthOutput *output;
    GError *error = NULL;
    GArray *array;
    QmiNasRadioInterface radio_interface;
    gint8 strength;
    gint32 io;
    QmiNasEvdoSinrLevel sinr_level;
    gint8 rsrq;
    gint16 rsrp;
    gint16 snr;

    output = qmi_client_nas_get_signal_strength_finish (client, res, &error);
    if (!output) {
        g_printerr ("error: operation failed: %s\n", error->message);
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_signal_strength_output_get_result (output, &error)) {
        g_printerr ("error: couldn't get signal strength: %s\n", error->message);
        g_error_free (error);
        qmi_message_nas_get_signal_strength_output_unref (output);
        shutdown (FALSE);
        return;
    }

    qmi_message_nas_get_signal_strength_output_get_signal_strength (output,
                                                                    &strength,
                                                                    &radio_interface,
                                                                    NULL);

    g_print ("[%s] Successfully got signal strength\n"
             "Current:\n"
             "\tNetwork '%s': '%d dBm'\n",
             qmi_device_get_path_display (ctx->device),
             qmi_nas_radio_interface_get_string (radio_interface),
             strength);

    /* Other signal strengths in other networks... */
    if (qmi_message_nas_get_signal_strength_output_get_strength_list (output, &array, NULL)) {
        guint i;

        g_print ("Other:\n");
        for (i = 0; i < array->len; i++) {
            QmiMessageNasGetSignalStrengthOutputStrengthListElement *element;

            element = &g_array_index (array, QmiMessageNasGetSignalStrengthOutputStrengthListElement, i);
            g_print ("\tNetwork '%s': '%d dBm'\n",
                     qmi_nas_radio_interface_get_string (element->radio_interface),
                     element->strength);
        }
    }

    /* RSSI... */
    if (qmi_message_nas_get_signal_strength_output_get_rssi_list (output, &array, NULL)) {
        guint i;

        g_print ("RSSI:\n");
        for (i = 0; i < array->len; i++) {
            QmiMessageNasGetSignalStrengthOutputRssiListElement *element;

            element = &g_array_index (array, QmiMessageNasGetSignalStrengthOutputRssiListElement, i);
            g_print ("\tNetwork '%s': '%d dBm'\n",
                     qmi_nas_radio_interface_get_string (element->radio_interface),
                     (-1) * element->rssi);
        }
    }

    /* ECIO... */
    if (qmi_message_nas_get_signal_strength_output_get_ecio_list (output, &array, NULL)) {
        guint i;

        g_print ("ECIO:\n");
        for (i = 0; i < array->len; i++) {
            QmiMessageNasGetSignalStrengthOutputEcioListElement *element;

            element = &g_array_index (array, QmiMessageNasGetSignalStrengthOutputEcioListElement, i);
            g_print ("\tNetwork '%s': '%.1lf dBm'\n",
                     qmi_nas_radio_interface_get_string (element->radio_interface),
                     (-0.5) * ((gdouble)element->ecio));
        }
    }

    /* IO... */
    if (qmi_message_nas_get_signal_strength_output_get_io (output, &io, NULL)) {
        g_print ("IO:\n"
                 "\tNetwork '%s': '%d dBm'\n",
                 qmi_nas_radio_interface_get_string (QMI_NAS_RADIO_INTERFACE_CDMA_1XEVDO),
                 io);
    }

    /* SINR level */
    if (qmi_message_nas_get_signal_strength_output_get_sinr (output, &sinr_level, NULL)) {
        g_print ("SINR:\n"
                 "\tNetwork '%s': (%u) '%.1lf dB'\n",
                 qmi_nas_radio_interface_get_string (QMI_NAS_RADIO_INTERFACE_CDMA_1XEVDO),
                 sinr_level, get_db_from_sinr_level (sinr_level));
    }

    /* RSRQ */
    if (qmi_message_nas_get_signal_strength_output_get_rsrq (output, &rsrq, &radio_interface, NULL)) {
        g_print ("RSRQ:\n"
                 "\tNetwork '%s': '%d dB'\n",
                 qmi_nas_radio_interface_get_string (radio_interface),
                 rsrq);
    }

    /* LTE SNR */
    if (qmi_message_nas_get_signal_strength_output_get_lte_snr (output, &snr, NULL)) {
        g_print ("SNR:\n"
                 "\tNetwork '%s': '%.1lf dB'\n",
                 qmi_nas_radio_interface_get_string (QMI_NAS_RADIO_INTERFACE_LTE),
                 (0.1) * ((gdouble)snr));
    }

    /* LTE RSRP */
    if (qmi_message_nas_get_signal_strength_output_get_lte_rsrp (output, &rsrp, NULL)) {
        g_print ("RSRP:\n"
                 "\tNetwork '%s': '%d dBm'\n",
                 qmi_nas_radio_interface_get_string (QMI_NAS_RADIO_INTERFACE_LTE),
                 rsrp);
    }

    /* Just skip others for now */

    qmi_message_nas_get_signal_strength_output_unref (output);
    shutdown (TRUE);
}

static void
get_serving_system_ready (QmiClientNas *client,
                          GAsyncResult *res)
{
    QmiMessageNasGetServingSystemOutput *output;
    GError *error = NULL;

    output = qmi_client_nas_get_serving_system_finish (client, res, &error);
    if (!output) {
        g_printerr ("error: operation failed: %s\n", error->message);
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_serving_system_output_get_result (output, &error)) {
        g_printerr ("error: couldn't get serving system: %s\n", error->message);
        g_error_free (error);
        qmi_message_nas_get_serving_system_output_unref (output);
        shutdown (FALSE);
        return;
    }

    g_print ("[%s] Successfully got serving system:\n",
             qmi_device_get_path_display (ctx->device));

    {
        QmiNasRegistrationState registration_state;
        QmiNasAttachState cs_attach_state;
        QmiNasAttachState ps_attach_state;
        QmiNasNetworkType selected_network;
        GArray *radio_interfaces;
        guint i;

        qmi_message_nas_get_serving_system_output_get_serving_system (
            output,
            &registration_state,
            &cs_attach_state,
            &ps_attach_state,
            &selected_network,
            &radio_interfaces,
            NULL);

        g_print ("\tRegistration state: '%s'\n"
                 "\tCS: '%s'\n"
                 "\tPS: '%s'\n"
                 "\tSelected network: '%s'\n"
                 "\tRadio interfaces: '%u'",
                 qmi_nas_registration_state_get_string (registration_state),
                 qmi_nas_attach_state_get_string (cs_attach_state),
                 qmi_nas_attach_state_get_string (ps_attach_state),
                 qmi_nas_network_type_get_string (selected_network),
                 radio_interfaces->len);

        for (i = 0; i < radio_interfaces->len; i++) {
            QmiNasRadioInterface iface;

            iface = g_array_index (radio_interfaces, QmiNasRadioInterface, i);
            g_print ("\t\t[%u]: '%s'\n", i, qmi_nas_radio_interface_get_string (iface));
        }
    }

    {
        QmiNasRoamingIndicatorStatus roaming;

        if (qmi_message_nas_get_serving_system_output_get_roaming_indicator (
                output,
                &roaming,
                NULL)) {
            g_print ("\tRoaming status: '%s'\n",
                     qmi_nas_roaming_indicator_status_get_string (roaming));
        }
    }

    {
        GArray *data_service_capability;

        if (qmi_message_nas_get_serving_system_output_get_data_service_capability (
                output,
                &data_service_capability,
                NULL)) {
            guint i;

            g_print ("\tData service capabilities: '%u'\n",
                     data_service_capability->len);

            for (i = 0; i < data_service_capability->len; i++) {
                QmiNasDataCapability cap;

                cap = g_array_index (data_service_capability, QmiNasDataCapability, i);
                g_print ("\t\t[%u]: '%s'\n", i, qmi_nas_data_capability_get_string (cap));
            }
        }
    }

    {
        guint16 current_plmn_mcc;
        guint16 current_plmn_mnc;
        const gchar *current_plmn_description;

        if (qmi_message_nas_get_serving_system_output_get_current_plmn (
                output,
                &current_plmn_mcc,
                &current_plmn_mnc,
                &current_plmn_description,
                NULL)) {
            g_print ("\tCurrent PLMN:\n"
                     "\t\tMCC: '%" G_GUINT16_FORMAT"'\n"
                     "\t\tMNC: '%" G_GUINT16_FORMAT"'\n"
                     "\t\tDescription: '%s'",
                     current_plmn_mcc,
                     current_plmn_mnc,
                     current_plmn_description);
        }
    }

    {
        guint16 sid;
        guint16 nid;

        if (qmi_message_nas_get_serving_system_output_get_cdma_system_id (
                output,
                &sid,
                &nid,
                NULL)) {
            g_print ("\tCDMA System ID:\n"
                     "\t\tSID: '%" G_GUINT16_FORMAT"'\n"
                     "\t\tESN: '%" G_GUINT16_FORMAT"'\n",
                     sid, nid);
        }
    }

    {
        guint16 id;
        gint32 latitude;
        gint32 longitude;

        if (qmi_message_nas_get_serving_system_output_get_cdma_base_station_info (
                output,
                &id,
                &latitude,
                &longitude,
                NULL)) {
            gdouble latitude_degrees;
            gdouble longitude_degrees;

            /* TODO: give degrees, minutes, seconds */
            latitude_degrees = ((gdouble)latitude * 0.25)/3600.0;
            longitude_degrees = ((gdouble)longitude * 0.25)/3600.0;

            g_print ("\tCDMA Base station info:\n"
                     "\t\tBase station ID: '%" G_GUINT16_FORMAT"'\n"
                     "\t\tLatitude: '%lf'º\n"
                     "\t\tLongitude: '%lf'º\n",
                     id, latitude_degrees, longitude_degrees);
        }
    }

    {
        GArray *roaming_indicators;

        if (qmi_message_nas_get_serving_system_output_get_roaming_indicator_list (
                output,
                &roaming_indicators,
                NULL)) {
            guint i;

            g_print ("\tRoaming indicators: '%u'\n",
                     roaming_indicators->len);

            for (i = 0; i < roaming_indicators->len; i++) {
                QmiMessageNasGetServingSystemOutputRoamingIndicatorListElement *element;

                element = &g_array_index (roaming_indicators, QmiMessageNasGetServingSystemOutputRoamingIndicatorListElement, i);
                g_print ("\t\t[%u]: '%s' (%s)\n",
                         i,
                         qmi_nas_roaming_indicator_status_get_string (element->roaming_indicator),
                         qmi_nas_radio_interface_get_string (element->radio_interface));
            }
        }
    }

    {
        QmiNasRoamingIndicatorStatus roaming;

        if (qmi_message_nas_get_serving_system_output_get_default_roaming_indicator (
                output,
                &roaming,
                NULL)) {
            g_print ("\tDefault roaming status: '%s'\n",
                     qmi_nas_roaming_indicator_status_get_string (roaming));
        }
    }

    {
        guint8 leap_seconds;
        gint8 local_time_offset;
        gboolean daylight_saving_time;

        if (qmi_message_nas_get_serving_system_output_get_time_zone_3gpp2 (
                output,
                &leap_seconds,
                &local_time_offset,
                &daylight_saving_time,
                NULL)) {
            g_print ("\t3GPP2 time zone:\n"
                     "\t\tLeap seconds: '%u' seconds\n"
                     "\t\tLocal time offset: '%d' minutes\n"
                     "\t\tDaylight saving time: '%s'\n",
                     leap_seconds,
                     (gint)local_time_offset * 30,
                     daylight_saving_time ? "yes" : "no");
        }
    }

    {
        guint8 cdma_p_rev;

        if (qmi_message_nas_get_serving_system_output_get_cdma_p_rev (
                output,
                &cdma_p_rev,
                NULL)) {
            g_print ("\tCDMA P_Rev: '%u'\n", cdma_p_rev);
        }
    }

    {
        gint8 time_zone;

        if (qmi_message_nas_get_serving_system_output_get_time_zone_3gpp (
                output,
                &time_zone,
                NULL)) {
            g_print ("\t3GPP time zone offset: '%d' minutes\n",
                     (gint)time_zone * 15);
        }
    }

    {
        guint8 adjustment;

        if (qmi_message_nas_get_serving_system_output_get_daylight_saving_time_adjustment_3gpp (
                output,
                &adjustment,
                NULL)) {
            g_print ("\t3GPP daylight saving time adjustment: '%u' hours\n",
                     adjustment);
        }
    }

    {
        guint16 lac;

        if (qmi_message_nas_get_serving_system_output_get_lac_3gpp (
                output,
                &lac,
                NULL)) {
            g_print ("\t3GPP location area code: '%" G_GUINT16_FORMAT"'\n", lac);
        }
    }

    {
        guint32 cid;

        if (qmi_message_nas_get_serving_system_output_get_cid_3gpp (
                output,
                &cid,
                NULL)) {
            g_print ("\t3GPP cell ID: '%u'\n", cid);
        }
    }

    {
        gboolean concurrent;

        if (qmi_message_nas_get_serving_system_output_get_concurrent_service_info_3gpp2 (
                output,
                &concurrent,
                NULL)) {
            g_print ("\t3GPP2 concurrent service info: '%s'\n",
                     concurrent ? "available" : "not available");
        }
    }

    {
        gboolean prl;

        if (qmi_message_nas_get_serving_system_output_get_prl_indicator_3gpp2 (
                output,
                &prl,
                NULL)) {
            g_print ("\t3GPP2 PRL indicator: '%s'\n",
                     prl ? "system in PRL" : "system not in PRL");
        }
    }

    {
        gboolean supported;

        if (qmi_message_nas_get_serving_system_output_get_dual_transfer_mode_supported (
                output,
                &supported,
                NULL)) {
            g_print ("\tDual transfer mode: '%s'\n",
                     supported ? "supported" : "not supported");
        }
    }

    {
        QmiNasServiceStatus status;
        QmiNasNetworkServiceDomain capability;
        QmiNasServiceStatus hdr_status;
        gboolean hdr_hybrid;
        gboolean forbidden;

        if (qmi_message_nas_get_serving_system_output_get_detailed_service_status (
                output,
                &status,
                &capability,
                &hdr_status,
                &hdr_hybrid,
                &forbidden,
                NULL)) {
            g_print ("\tDetailed status:\n"
                     "\t\tStatus: '%s'\n"
                     "\t\tCapability: '%s'\n"
                     "\t\tHDR Status: '%s'\n"
                     "\t\tHDR Hybrid: '%s'\n"
                     "\t\tForbidden: '%s'\n",
                     qmi_nas_service_status_get_string (status),
                     qmi_nas_network_service_domain_get_string (capability),
                     qmi_nas_service_status_get_string (hdr_status),
                     hdr_hybrid ? "yes" : "no",
                     forbidden ? "yes" : "no");
        }
    }

    {
        guint16 mcc;
        guint8 imsi_11_12;

        if (qmi_message_nas_get_serving_system_output_get_cdma_system_info (
                output,
                &mcc,
                &imsi_11_12,
                NULL)) {
            g_print ("\tCDMA system info:\n"
                     "\t\tMCC: '%" G_GUINT16_FORMAT"'\n"
                     "\t\tIMSI_11_12: '%u'\n",
                     mcc,
                     imsi_11_12);
        }
    }

    {
        QmiNasHdrPersonality personality;

        if (qmi_message_nas_get_serving_system_output_get_hdr_personality (
                output,
                &personality,
                NULL)) {
            g_print ("\tHDR personality: '%s'\n",
                     qmi_nas_hdr_personality_get_string (personality));
        }
    }

    {
        guint16 tac;

        if (qmi_message_nas_get_serving_system_output_get_lte_tac (
                output,
                &tac,
                NULL)) {
            g_print ("\tLTE tracking area code: '%" G_GUINT16_FORMAT"'\n", tac);
        }
    }

    {
        QmiNasCallBarringStatus cs_status;
        QmiNasCallBarringStatus ps_status;

        if (qmi_message_nas_get_serving_system_output_get_call_barring_status (
                output,
                &cs_status,
                &ps_status,
                NULL)) {
            g_print ("\tCall barring status:\n"
                     "\t\tCircuit switched: '%s'\n"
                     "\t\tPacket switched: '%s'\n",
                     qmi_nas_call_barring_status_get_string (cs_status),
                     qmi_nas_call_barring_status_get_string (ps_status));
        }
    }

    {
        guint16 code;

        if (qmi_message_nas_get_serving_system_output_get_umts_primary_scrambling_code (
                output,
                &code,
                NULL)) {
            g_print ("\tUMTS primary scrambling code: '%" G_GUINT16_FORMAT"'\n", code);
        }
    }

    {
        guint16 mcc;
        guint16 mnc;
        gboolean has_pcs_digit;

        if (qmi_message_nas_get_serving_system_output_get_mnc_pcs_digit_include_status (
                output,
                &mcc,
                &mnc,
                &has_pcs_digit,
                NULL)) {
            g_print ("\tFull operator code info:\n"
                     "\t\tMCC: '%" G_GUINT16_FORMAT"'\n"
                     "\t\tMNC: '%" G_GUINT16_FORMAT"'\n"
                     "\t\tMNC with PCS digit: '%s'\n",
                     mcc,
                     mnc,
                     has_pcs_digit ? "yes" : "no");
        }
    }

    qmi_message_nas_get_serving_system_output_unref (output);
    shutdown (TRUE);
}

static void
get_technology_preference_ready (QmiClientNas *client,
                                 GAsyncResult *res)
{
    QmiMessageNasGetTechnologyPreferenceOutput *output;
    GError *error = NULL;
    QmiNasRadioTechnologyPreference preference;
    QmiNasPreferenceDuration duration;
    gchar *preference_string;

    output = qmi_client_nas_get_technology_preference_finish (client, res, &error);
    if (!output) {
        g_printerr ("error: operation failed: %s\n", error->message);
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_technology_preference_output_get_result (output, &error)) {
        g_printerr ("error: couldn't get technology preference: %s\n", error->message);
        g_error_free (error);
        qmi_message_nas_get_technology_preference_output_unref (output);
        shutdown (FALSE);
        return;
    }

    qmi_message_nas_get_technology_preference_output_get_active (
        output,
        &preference,
        &duration,
        NULL);

    preference_string = qmi_nas_radio_technology_preference_build_string_from_mask (preference);
    g_print ("[%s] Successfully got technology preference\n"
             "\tActive: '%s', duration: '%s'\n",
             qmi_device_get_path_display (ctx->device),
             preference_string,
             qmi_nas_preference_duration_get_string (duration));
    g_free (preference_string);

    if (qmi_message_nas_get_technology_preference_output_get_persistent (
            output,
            &preference,
            NULL)) {
        preference_string = qmi_nas_radio_technology_preference_build_string_from_mask (preference);
        g_print ("\tPersistent: '%s', duration: '%s'\n",
                 qmi_device_get_path_display (ctx->device),
                 preference_string);
        g_free (preference_string);
    }

    qmi_message_nas_get_technology_preference_output_unref (output);
    shutdown (TRUE);
}

static void
get_system_selection_preference_ready (QmiClientNas *client,
                                 GAsyncResult *res)
{
    QmiMessageNasGetSystemSelectionPreferenceOutput *output;
    GError *error = NULL;
    gboolean emergency_mode;
    QmiNasRatModePreference mode_preference;
    QmiNasBandPreference band_preference;
    QmiNasLteBandPreference lte_band_preference;
    QmiNasTdScdmaBandPreference td_scdma_band_preference;
    QmiNasCdmaPrlPreference cdma_prl_preference;
    QmiNasRoamingPreference roaming_preference;
    QmiNasNetworkSelectionPreference network_selection_preference;
    QmiNasServiceDomainPreference service_domain_preference;
    QmiNasGsmWcdmaAcquisitionOrderPreference gsm_wcdma_acquisition_order_preference;
    guint16 mcc;
    guint16 mnc;
    gboolean has_pcs_digit;

    output = qmi_client_nas_get_system_selection_preference_finish (client, res, &error);
    if (!output) {
        g_printerr ("error: operation failed: %s\n", error->message);
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_system_selection_preference_output_get_result (output, &error)) {
        g_printerr ("error: couldn't get system_selection preference: %s\n", error->message);
        g_error_free (error);
        qmi_message_nas_get_system_selection_preference_output_unref (output);
        shutdown (FALSE);
        return;
    }

    g_print ("[%s] Successfully got system selection preference\n",
             qmi_device_get_path_display (ctx->device));

    if (qmi_message_nas_get_system_selection_preference_output_get_emergency_mode (
            output,
            &emergency_mode,
            NULL)) {
        g_print ("\tEmergency mode: '%s'\n",
                 emergency_mode ? "yes" : "no");
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_mode_preference (
            output,
            &mode_preference,
            NULL)) {
        gchar *str;

        str = qmi_nas_rat_mode_preference_build_string_from_mask (mode_preference);
        g_print ("\tMode preference: '%s'\n", str);
        g_free (str);
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_band_preference (
            output,
            &band_preference,
            NULL)) {
        gchar *str;

        str = qmi_nas_band_preference_build_string_from_mask (band_preference);
        g_print ("\tBand preference: '%s'\n", str);
        g_free (str);
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_lte_band_preference (
            output,
            &lte_band_preference,
            NULL)) {
        gchar *str;

        str = qmi_nas_lte_band_preference_build_string_from_mask (lte_band_preference);
        g_print ("\tLTE band preference: '%s'\n", str);
        g_free (str);
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_td_scdma_band_preference (
            output,
            &td_scdma_band_preference,
            NULL)) {
        gchar *str;

        str = qmi_nas_td_scdma_band_preference_build_string_from_mask (td_scdma_band_preference);
        g_print ("\tTD-SCDMA band preference: '%s'\n", str);
        g_free (str);
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_cdma_prl_preference (
            output,
            &cdma_prl_preference,
            NULL)) {
        g_print ("\tCDMA PRL preference: '%s'\n",
                 qmi_nas_cdma_prl_preference_get_string (cdma_prl_preference));
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_roaming_preference (
            output,
            &roaming_preference,
            NULL)) {
        g_print ("\tRoaming preference: '%s'\n",
                 qmi_nas_roaming_preference_get_string (roaming_preference));
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_network_selection_preference (
            output,
            &network_selection_preference,
            NULL)) {
        g_print ("\tNetwork selection preference: '%s'\n",
                 qmi_nas_network_selection_preference_get_string (network_selection_preference));
    }


    if (qmi_message_nas_get_system_selection_preference_output_get_service_domain_preference (
            output,
            &service_domain_preference,
            NULL)) {
        g_print ("\tService domain preference: '%s'\n",
                 qmi_nas_service_domain_preference_get_string (service_domain_preference));
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_gsm_wcdma_acquisition_order_preference (
            output,
            &gsm_wcdma_acquisition_order_preference,
            NULL)) {
        g_print ("\tService selection preference: '%s'\n",
                 qmi_nas_gsm_wcdma_acquisition_order_preference_get_string (gsm_wcdma_acquisition_order_preference));
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_manual_network_selection (
            output,
            &mcc,
            &mnc,
            &has_pcs_digit,
            NULL)) {
        g_print ("\tManual network selection:\n"
                 "\t\tMCC: '%" G_GUINT16_FORMAT"'\n"
                 "\t\tMNC: '%" G_GUINT16_FORMAT"'\n"
                 "\t\tMCC with PCS digit: '%s'\n",
                 mcc,
                 mnc,
                 has_pcs_digit ? "yes" : "no");
    }

    qmi_message_nas_get_system_selection_preference_output_unref (output);
    shutdown (TRUE);
}

static void
network_scan_ready (QmiClientNas *client,
                    GAsyncResult *res)
{
    QmiMessageNasNetworkScanOutput *output;
    GError *error = NULL;
    GArray *array;

    output = qmi_client_nas_network_scan_finish (client, res, &error);
    if (!output) {
        g_printerr ("error: operation failed: %s\n", error->message);
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_network_scan_output_get_result (output, &error)) {
        g_printerr ("error: couldn't scan networks: %s\n", error->message);
        g_error_free (error);
        qmi_message_nas_network_scan_output_unref (output);
        shutdown (FALSE);
        return;
    }

    g_print ("[%s] Successfully scanned networks\n",
             qmi_device_get_path_display (ctx->device));

    array = NULL;
    if (qmi_message_nas_network_scan_output_get_network_information (output, &array, NULL)) {
        guint i;

        for (i = 0; i < array->len; i++) {
            QmiMessageNasNetworkScanOutputNetworkInformationElement *element;
            gchar *status_str;

            element = &g_array_index (array, QmiMessageNasNetworkScanOutputNetworkInformationElement, i);
            status_str = qmi_nas_network_status_build_string_from_mask (element->network_status);
            g_print ("Network [%u]:\n"
                     "\tMCC: '%" G_GUINT16_FORMAT"'\n"
                     "\tMNC: '%" G_GUINT16_FORMAT"'\n"
                     "\tStatus: '%s'\n"
                     "\tDescription: '%s'\n",
                     i,
                     element->mcc,
                     element->mnc,
                     status_str,
                     element->description);
            g_free (status_str);
        }
    }

    array = NULL;
    if (qmi_message_nas_network_scan_output_get_radio_access_technology (output, &array, NULL)) {
        guint i;

        for (i = 0; i < array->len; i++) {
            QmiMessageNasNetworkScanOutputRadioAccessTechnologyElement *element;

            element = &g_array_index (array, QmiMessageNasNetworkScanOutputRadioAccessTechnologyElement, i);
            g_print ("Network [%u]:\n"
                     "\tMCC: '%" G_GUINT16_FORMAT"'\n"
                     "\tMNC: '%" G_GUINT16_FORMAT"'\n"
                     "\tRAT: '%s'\n",
                     i,
                     element->mcc,
                     element->mnc,
                     qmi_nas_radio_interface_get_string (element->radio_interface));
        }
    }

    array = NULL;
    if (qmi_message_nas_network_scan_output_get_mnc_pcs_digit_include_status (output, &array, NULL)) {
        guint i;

        for (i = 0; i < array->len; i++) {
            QmiMessageNasNetworkScanOutputMncPcsDigitIncludeStatusElement *element;

            element = &g_array_index (array, QmiMessageNasNetworkScanOutputMncPcsDigitIncludeStatusElement, i);
            g_print ("Network [%u]:\n"
                     "\tMCC: '%" G_GUINT16_FORMAT"'\n"
                     "\tMNC: '%" G_GUINT16_FORMAT"'\n"
                     "\tMCC with PCS digit: '%s'\n",
                     i,
                     element->mcc,
                     element->mnc,
                     element->includes_pcs_digit ? "yes" : "no");
        }
    }

    qmi_message_nas_network_scan_output_unref (output);
    shutdown (TRUE);
}

static void
reset_ready (QmiClientNas *client,
             GAsyncResult *res)
{
    QmiMessageNasResetOutput *output;
    GError *error = NULL;

    output = qmi_client_nas_reset_finish (client, res, &error);
    if (!output) {
        g_printerr ("error: operation failed: %s\n", error->message);
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_reset_output_get_result (output, &error)) {
        g_printerr ("error: couldn't reset the NAS service: %s\n", error->message);
        g_error_free (error);
        qmi_message_nas_reset_output_unref (output);
        shutdown (FALSE);
        return;
    }

    g_print ("[%s] Successfully performed NAS service reset\n",
             qmi_device_get_path_display (ctx->device));

    qmi_message_nas_reset_output_unref (output);
    shutdown (TRUE);
}

static gboolean
noop_cb (gpointer unused)
{
    shutdown (TRUE);
    return FALSE;
}

void
qmicli_nas_run (QmiDevice *device,
                QmiClientNas *client,
                GCancellable *cancellable)
{
    /* Initialize context */
    ctx = g_slice_new (Context);
    ctx->device = g_object_ref (device);
    ctx->client = g_object_ref (client);
    if (cancellable)
        ctx->cancellable = g_object_ref (cancellable);

    /* Request to get signal strength? */
    if (get_signal_strength_flag) {
        QmiMessageNasGetSignalStrengthInput *input;

        input = get_signal_strength_input_create ();

        g_debug ("Asynchronously getting signal strength...");
        qmi_client_nas_get_signal_strength (ctx->client,
                                            input,
                                            10,
                                            ctx->cancellable,
                                            (GAsyncReadyCallback)get_signal_strength_ready,
                                            NULL);
        qmi_message_nas_get_signal_strength_input_unref (input);
        return;
    }

    /* Request to get signal info? */
    if (get_signal_info_flag) {
        g_debug ("Asynchronously getting signal info...");
        qmi_client_nas_get_signal_info (ctx->client,
                                        NULL,
                                        10,
                                        ctx->cancellable,
                                        (GAsyncReadyCallback)get_signal_info_ready,
                                        NULL);
        return;
    }

    /* Request to get serving system? */
    if (get_serving_system_flag) {
        g_debug ("Asynchronously getting serving system...");
        qmi_client_nas_get_serving_system (ctx->client,
                                           NULL,
                                           10,
                                           ctx->cancellable,
                                           (GAsyncReadyCallback)get_serving_system_ready,
                                           NULL);
        return;
    }

    /* Request to get technology preference? */
    if (get_technology_preference_flag) {
        g_debug ("Asynchronously getting technology preference...");
        qmi_client_nas_get_technology_preference (ctx->client,
                                                  NULL,
                                                  10,
                                                  ctx->cancellable,
                                                  (GAsyncReadyCallback)get_technology_preference_ready,
                                                  NULL);
        return;
    }

    /* Request to get system_selection preference? */
    if (get_system_selection_preference_flag) {
        g_debug ("Asynchronously getting system selection preference...");
        qmi_client_nas_get_system_selection_preference (ctx->client,
                                                        NULL,
                                                        10,
                                                        ctx->cancellable,
                                                        (GAsyncReadyCallback)get_system_selection_preference_ready,
                                                        NULL);
        return;
    }

    /* Request to scan networks? */
    if (network_scan_flag) {
        g_debug ("Asynchronously scanning networks...");
        qmi_client_nas_network_scan (ctx->client,
                                     NULL,
                                     300, /* this operation takes a lot of time! */
                                     ctx->cancellable,
                                     (GAsyncReadyCallback)network_scan_ready,
                                     NULL);
        return;
    }

    /* Request to reset NAS service? */
    if (reset_flag) {
        g_debug ("Asynchronously resetting NAS service...");
        qmi_client_nas_reset (ctx->client,
                              NULL,
                              10,
                              ctx->cancellable,
                              (GAsyncReadyCallback)reset_ready,
                              NULL);
        return;
    }

    /* Just client allocate/release? */
    if (noop_flag) {
        g_idle_add (noop_cb, NULL);
        return;
    }

    g_warn_if_reached ();
}