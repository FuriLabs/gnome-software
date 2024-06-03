/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2024 Red Hat <www.redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/**
 * SECTION:gs-plugin-job-download-upgrade
 * @short_description: A plugin job on an app
 *
 * #GsPluginJobDownloadUpgrade is a #GsPluginJob to start download
 * of a system upgrade of @app.
 *
 * This class is a wrapper around #GsPluginClass.download_upgrade_async
 * calling it for all loaded plugins.
 *
 * Since: 47
 */

#include "config.h"

#include <glib.h>
#include <glib-object.h>

#include "gs-app.h"
#include "gs-enums.h"
#include "gs-plugin-job.h"
#include "gs-plugin-job-download-upgrade.h"
#include "gs-plugin-job-private.h"
#include "gs-plugin-types.h"

struct _GsPluginJobDownloadUpgrade
{
	GsPluginJob parent;

	/* Input arguments. */
	GsApp *app;  /* (owned) (not nullable) */
	GsPluginDownloadUpgradeFlags flags;

	/* In-progress data. */
	GError *saved_error;  /* (owned) (nullable) */
	guint n_pending_ops;
};

G_DEFINE_TYPE (GsPluginJobDownloadUpgrade, gs_plugin_job_download_upgrade, GS_TYPE_PLUGIN_JOB)

typedef enum {
	PROP_FLAGS = 1,
	PROP_APP,
} GsPluginJobDownloadUpgradeProperty;

static GParamSpec *props[PROP_APP + 1] = { NULL, };

static void
gs_plugin_job_download_upgrade_dispose (GObject *object)
{
	GsPluginJobDownloadUpgrade *self = GS_PLUGIN_JOB_DOWNLOAD_UPGRADE (object);

	g_assert (self->saved_error == NULL);
	g_assert (self->n_pending_ops == 0);

	g_clear_object (&self->app);

	G_OBJECT_CLASS (gs_plugin_job_download_upgrade_parent_class)->dispose (object);
}

static void
gs_plugin_job_download_upgrade_get_property (GObject *object,
					     guint prop_id,
					     GValue *value,
					     GParamSpec *pspec)
{
	GsPluginJobDownloadUpgrade *self = GS_PLUGIN_JOB_DOWNLOAD_UPGRADE (object);

	switch ((GsPluginJobDownloadUpgradeProperty) prop_id) {
	case PROP_FLAGS:
		g_value_set_flags (value, self->flags);
		break;
	case PROP_APP:
		g_value_set_object (value, self->app);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gs_plugin_job_download_upgrade_set_property (GObject *object,
					     guint prop_id,
					     const GValue *value,
					     GParamSpec *pspec)
{
	GsPluginJobDownloadUpgrade *self = GS_PLUGIN_JOB_DOWNLOAD_UPGRADE (object);

	switch ((GsPluginJobDownloadUpgradeProperty) prop_id) {
	case PROP_FLAGS:
		/* Construct only. */
		g_assert (self->flags == 0);
		self->flags = g_value_get_flags (value);
		g_object_notify_by_pspec (object, props[prop_id]);
		break;
	case PROP_APP:
		/* Construct only. */
		g_assert (self->app == NULL);
		self->app = g_value_dup_object (value);
		g_assert (self->app != NULL);
		g_object_notify_by_pspec (object, props[prop_id]);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void plugin_app_func_cb (GObject      *source_object,
				GAsyncResult *result,
				gpointer      user_data);
static void finish_op (GTask  *task,
                       GError *error);

static void
gs_plugin_job_download_upgrade_run_async (GsPluginJob         *job,
					  GsPluginLoader      *plugin_loader,
					  GCancellable        *cancellable,
					  GAsyncReadyCallback  callback,
					  gpointer             user_data)
{
	GsPluginJobDownloadUpgrade *self = GS_PLUGIN_JOB_DOWNLOAD_UPGRADE (job);
	g_autoptr(GTask) task = NULL;
	GPtrArray *plugins;  /* (element-type GsPlugin) */
	gboolean anything_ran = FALSE;
	g_autoptr(GError) local_error = NULL;

	task = g_task_new (job, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_job_download_upgrade_run_async);
	g_task_set_task_data (task, g_object_ref (plugin_loader), (GDestroyNotify) g_object_unref);

	/* run each plugin, keeping a counter of pending operations which is
	 * initialised to 1 until all the operations are started */
	self->n_pending_ops = 1;
	plugins = gs_plugin_loader_get_plugins (plugin_loader);

	for (guint i = 0; i < plugins->len; i++) {
		GsPlugin *plugin = g_ptr_array_index (plugins, i);
		GsPluginClass *plugin_class = GS_PLUGIN_GET_CLASS (plugin);

		if (!gs_plugin_get_enabled (plugin))
			continue;
		if (plugin_class->download_upgrade_async == NULL)
			continue;

		/* at least one plugin supports this vfunc */
		anything_ran = TRUE;

		/* Handle cancellation */
		if (g_cancellable_set_error_if_cancelled (cancellable, &local_error))
			break;

		/* run the plugin */
		self->n_pending_ops++;
		plugin_class->download_upgrade_async (plugin, self->app, self->flags, cancellable, plugin_app_func_cb, g_object_ref (task));
	}

	if (!anything_ran)
		g_debug ("no plugin could handle app operation");

	finish_op (task, g_steal_pointer (&local_error));
}

static void
plugin_app_func_cb (GObject      *source_object,
		    GAsyncResult *result,
		    gpointer      user_data)
{
	GsPlugin *plugin = GS_PLUGIN (source_object);
	GsPluginClass *plugin_class = GS_PLUGIN_GET_CLASS (plugin);
	g_autoptr(GTask) task = G_TASK (user_data);
	gboolean success;
	g_autoptr(GError) local_error = NULL;

	success = plugin_class->download_upgrade_finish (plugin, result, &local_error);
	gs_plugin_status_update (plugin, NULL, GS_PLUGIN_STATUS_FINISHED);

	g_assert (success || local_error != NULL);

	finish_op (task, g_steal_pointer (&local_error));
}

/* @error is (transfer full) if non-%NULL */
static void
finish_op (GTask  *task,
           GError *error)
{
	GsPluginJobDownloadUpgrade *self = g_task_get_source_object (task);
	g_autoptr(GError) error_owned = g_steal_pointer (&error);
	g_autofree gchar *job_debug = NULL;

	if (error_owned != NULL && self->saved_error == NULL)
		self->saved_error = g_steal_pointer (&error_owned);
	else if (error_owned != NULL)
		g_debug ("Additional error while downloading upgrade: %s", error_owned->message);

	g_assert (self->n_pending_ops > 0);
	self->n_pending_ops--;

	if (self->n_pending_ops > 0)
		return;

	/* show elapsed time */
	job_debug = gs_plugin_job_to_string (GS_PLUGIN_JOB (self));
	g_debug ("%s", job_debug);

	if (self->saved_error != NULL)
		g_task_return_error (task, g_steal_pointer (&self->saved_error));
	else
		g_task_return_boolean (task, TRUE);
	g_signal_emit_by_name (G_OBJECT (self), "completed");
}

static gboolean
gs_plugin_job_download_upgrade_run_finish (GsPluginJob   *self,
					   GAsyncResult  *result,
					   GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_job_download_upgrade_class_init (GsPluginJobDownloadUpgradeClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPluginJobClass *job_class = GS_PLUGIN_JOB_CLASS (klass);

	object_class->dispose = gs_plugin_job_download_upgrade_dispose;
	object_class->get_property = gs_plugin_job_download_upgrade_get_property;
	object_class->set_property = gs_plugin_job_download_upgrade_set_property;

	job_class->run_async = gs_plugin_job_download_upgrade_run_async;
	job_class->run_finish = gs_plugin_job_download_upgrade_run_finish;

	/**
	 * GsPluginJobDownloadUpgrade:app: (not nullable)
	 *
	 * A #GsApp describing the app to run the operation on.
	 *
	 * Since: 47
	 */
	props[PROP_APP] =
		g_param_spec_object ("app", "App",
				     "A #GsApp describing the app to run the operation on.",
				     GS_TYPE_APP,
				     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
				     G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * GsPluginJobDownloadUpgrade:flags:
	 *
	 * Flags affecting how the operation runs.
	 *
	 * Since: 47
	 */
	props[PROP_FLAGS] =
		g_param_spec_flags ("flags", "Flags",
				    "Flags affecting how the operation runs.",
				    GS_TYPE_PLUGIN_DOWNLOAD_UPGRADE_FLAGS,
				    GS_PLUGIN_DOWNLOAD_UPGRADE_FLAGS_NONE,
				    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
				    G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

	g_object_class_install_properties (object_class, G_N_ELEMENTS (props), props);
}

static void
gs_plugin_job_download_upgrade_init (GsPluginJobDownloadUpgrade *self)
{
}

/**
 * gs_plugin_job_download_upgrade_new:
 * @app: (not nullable) (transfer none): an app to run the operation on
 * @flags: flags affecting how the operation runs
 *
 * Create a new #GsPluginJobDownloadUpgrade to download a system upgrade represented by @app.
 *
 * Returns: (transfer full): a new #GsPluginJobDownloadUpgrade
 * Since: 47
 */
GsPluginJob *
gs_plugin_job_download_upgrade_new (GsApp *app,
				    GsPluginDownloadUpgradeFlags flags)
{
	g_return_val_if_fail (GS_IS_APP (app), NULL);

	return g_object_new (GS_TYPE_PLUGIN_JOB_DOWNLOAD_UPGRADE,
			     "app", app,
			     "flags", flags,
			     NULL);
}
