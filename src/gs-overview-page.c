/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2013-2017 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2014-2018 Kalev Lember <klember@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "config.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <math.h>

#include "gs-shell.h"
#include "gs-overview-page.h"
#include "gs-app-list-private.h"
#include "gs-featured-carousel.h"
#include "gs-category-tile.h"
#include "gs-common.h"
#include "gs-summary-tile.h"

/* Chosen as it has 2 and 3 as factors, so will form an even 2-column and
 * 3-column layout. */
#define N_TILES 12

struct _GsOverviewPage
{
	GsPage			 parent_instance;

	GsPluginLoader		*plugin_loader;
	GCancellable		*cancellable;
	gboolean		 cache_valid;
	GsShell			*shell;
	gint			 action_cnt;
	gboolean		 loading_featured;
	gboolean		 loading_curated;
	gboolean		 loading_deployment_featured;
	gboolean		 loading_recent;
	gboolean		 loading_categories;
	gboolean		 empty;
	gboolean		 featured_overwritten;
	GHashTable		*category_hash;		/* id : GsCategory */
	GsFedoraThirdParty	*third_party;
	gboolean		 third_party_needs_question;
	gchar		       **deployment_featured;

	GtkWidget		*infobar_third_party;
	GtkWidget		*label_third_party;
	GtkWidget		*featured_carousel;
	GtkWidget		*box_overview;
	GtkWidget		*box_curated;
	GtkWidget		*box_recent;
	GtkWidget		*box_deployment_featured;
	GtkWidget		*flowbox_categories;
	GtkWidget		*flowbox_iconless_categories;
	GtkWidget		*iconless_categories_heading;
	GtkWidget		*curated_heading;
	GtkWidget		*recent_heading;
	GtkWidget		*deployment_featured_heading;
	GtkWidget		*scrolledwindow_overview;
	GtkWidget		*stack_overview;
};

G_DEFINE_TYPE (GsOverviewPage, gs_overview_page, GS_TYPE_PAGE)

typedef enum {
	PROP_VADJUSTMENT = 1,
	PROP_TITLE,
} GsOverviewPageProperty;

enum {
	SIGNAL_REFRESHED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

static void
gs_overview_page_invalidate (GsOverviewPage *self)
{
	self->cache_valid = FALSE;
}

static void
app_tile_clicked (GsAppTile *tile, gpointer data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (data);
	GsApp *app;

	app = gs_app_tile_get_app (tile);
	gs_shell_show_app (self->shell, app);
}

static void
featured_carousel_app_clicked_cb (GsFeaturedCarousel *carousel,
                                  GsApp              *app,
                                  gpointer            user_data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (user_data);

	gs_shell_show_app (self->shell, app);
}

static void
gs_overview_page_decrement_action_cnt (GsOverviewPage *self)
{
	/* every job increments this */
	if (self->action_cnt == 0) {
		g_warning ("action_cnt already zero!");
		return;
	}
	if (--self->action_cnt > 0)
		return;

	/* all done */
	self->cache_valid = TRUE;
	g_signal_emit (self, signals[SIGNAL_REFRESHED], 0);
	self->loading_categories = FALSE;
	self->loading_deployment_featured = FALSE;
	self->loading_featured = FALSE;
	self->loading_curated = FALSE;
	self->loading_recent = FALSE;
}

static void
gs_overview_page_get_curated_cb (GObject *source_object,
                                 GAsyncResult *res,
                                 gpointer user_data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (user_data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	guint i;
	GsApp *app;
	GtkWidget *tile;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	/* get curated apps */
	list = gs_plugin_loader_job_process_finish (plugin_loader, res, &error);
	if (list == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get curated apps: %s", error->message);
		goto out;
	}

	/* not enough to show */
	if (gs_app_list_length (list) < N_TILES) {
		g_warning ("Only %u apps for curated list, hiding",
		           gs_app_list_length (list));
		gtk_widget_set_visible (self->box_curated, FALSE);
		gtk_widget_set_visible (self->curated_heading, FALSE);
		goto out;
	}

	g_assert (gs_app_list_length (list) == N_TILES);

	gs_widget_remove_all (self->box_curated, (GsRemoveFunc) gtk_flow_box_remove);

	for (i = 0; i < gs_app_list_length (list); i++) {
		app = gs_app_list_index (list, i);
		tile = gs_summary_tile_new (app);
		g_signal_connect (tile, "clicked",
			  G_CALLBACK (app_tile_clicked), self);
		gtk_flow_box_insert (GTK_FLOW_BOX (self->box_curated), tile, -1);
	}
	gtk_widget_set_visible (self->box_curated, TRUE);
	gtk_widget_set_visible (self->curated_heading, TRUE);

	self->empty = FALSE;

out:
	gs_overview_page_decrement_action_cnt (self);
}

static gint
gs_overview_page_sort_recent_cb (GsApp *app1,
				 GsApp *app2,
				 gpointer user_data)
{
	if (gs_app_get_release_date (app1) < gs_app_get_release_date (app2))
		return 1;
	if (gs_app_get_release_date (app1) == gs_app_get_release_date (app2))
		return g_strcmp0 (gs_app_get_name (app1), gs_app_get_name (app2));
	return -1;
}

static gboolean
gs_overview_page_filter_recent_cb (GsApp    *app,
                                   gpointer  user_data)
{
	return (!gs_app_has_quirk (app, GS_APP_QUIRK_COMPULSORY) &&
		gs_app_get_kind (app) == AS_COMPONENT_KIND_DESKTOP_APP);
}

static void
gs_overview_page_get_recent_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (user_data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	guint i;
	GsApp *app;
	GtkWidget *tile;
	GtkWidget *child;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	/* get recent apps */
	list = gs_plugin_loader_job_process_finish (plugin_loader, res, &error);
	if (list == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get recent apps: %s", error->message);
		goto out;
	}

	/* not enough to show */
	if (gs_app_list_length (list) < N_TILES) {
		g_warning ("Only %u apps for recent list, hiding",
			   gs_app_list_length (list));
		gtk_widget_set_visible (self->box_recent, FALSE);
		gtk_widget_set_visible (self->recent_heading, FALSE);
		goto out;
	}

	g_assert (gs_app_list_length (list) <= N_TILES);

	gs_widget_remove_all (self->box_recent, (GsRemoveFunc) gtk_flow_box_remove);

	for (i = 0; i < gs_app_list_length (list); i++) {
		app = gs_app_list_index (list, i);
		tile = gs_summary_tile_new (app);
		g_signal_connect (tile, "clicked",
			  G_CALLBACK (app_tile_clicked), self);
		child = gtk_flow_box_child_new ();
		/* Manually creating the child is needed to avoid having it be
		 * focusable but non activatable, and then have the child
		 * focusable and activatable, which is annoying and confusing.
		 */
		gtk_widget_set_can_focus (child, FALSE);
		gtk_widget_show (child);
		gtk_flow_box_child_set_child (GTK_FLOW_BOX_CHILD (child), tile);
		gtk_flow_box_insert (GTK_FLOW_BOX (self->box_recent), child, -1);
	}
	gtk_widget_set_visible (self->box_recent, TRUE);
	gtk_widget_set_visible (self->recent_heading, TRUE);

	self->empty = FALSE;

out:
	gs_overview_page_decrement_action_cnt (self);
}

static gboolean
filter_hi_res_icon (GsApp *app, gpointer user_data)
{
	g_autoptr(GIcon) icon = NULL;
	GtkWidget *overview_page = GTK_WIDGET (user_data);

	/* This is the minimum icon size needed by `GsFeatureTile`. */
	icon = gs_app_get_icon_for_size (app,
					 128,
					 gtk_widget_get_scale_factor (overview_page),
					 NULL);

	/* Returning TRUE means to keep the app in the list */
	return (icon != NULL);
}

static void
gs_overview_page_get_featured_cb (GObject *source_object,
                                  GAsyncResult *res,
                                  gpointer user_data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (user_data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	list = gs_plugin_loader_job_process_finish (plugin_loader, res, &error);
	if (g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) ||
	    g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		goto out;

	if (self->featured_overwritten) {
		g_debug ("Skipping set of featured apps, because being overwritten");
		goto out;
	}

	if (list == NULL || gs_app_list_length (list) == 0) {
		g_warning ("failed to get featured apps: %s",
			   (error != NULL) ? error->message : "no apps to show");
		gtk_widget_set_visible (self->featured_carousel, FALSE);
		goto out;
	}

	gtk_widget_set_visible (self->featured_carousel, gs_app_list_length (list) > 0);
	gs_featured_carousel_set_apps (GS_FEATURED_CAROUSEL (self->featured_carousel), list);

	self->empty = self->empty && (gs_app_list_length (list) == 0);

out:
	gs_overview_page_decrement_action_cnt (self);
}

static void
gs_overview_page_get_deployment_featured_cb (GObject *source_object,
					     GAsyncResult *res,
					     gpointer user_data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (user_data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	guint i;
	GsApp *app;
	GtkWidget *tile;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	/* get deployment-featured apps */
	list = gs_plugin_loader_job_process_finish (plugin_loader, res, &error);
	if (list == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("failed to get deployment-featured apps: %s", error->message);
		goto out;
	}

	/* not enough to show */
	if (gs_app_list_length (list) < N_TILES) {
		g_warning ("Only %u apps for deployment-featured list, hiding",
		           gs_app_list_length (list));
		gtk_widget_set_visible (self->box_deployment_featured, FALSE);
		gtk_widget_set_visible (self->deployment_featured_heading, FALSE);
		goto out;
	}

	g_assert (gs_app_list_length (list) == N_TILES);
	gs_widget_remove_all (self->box_deployment_featured, (GsRemoveFunc) gtk_flow_box_remove);

	for (i = 0; i < gs_app_list_length (list); i++) {
		app = gs_app_list_index (list, i);
		tile = gs_summary_tile_new (app);
		g_signal_connect (tile, "clicked",
			  G_CALLBACK (app_tile_clicked), self);
		gtk_flow_box_insert (GTK_FLOW_BOX (self->box_deployment_featured), tile, -1);
	}
	gtk_widget_set_visible (self->box_deployment_featured, TRUE);
	gtk_widget_set_visible (self->deployment_featured_heading, TRUE);

	self->empty = FALSE;

 out:
	gs_overview_page_decrement_action_cnt (self);
}

static void
category_tile_clicked (GsCategoryTile *tile, gpointer data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (data);
	GsCategory *category;

	category = gs_category_tile_get_category (tile);
	gs_shell_show_category (self->shell, category);
}

typedef struct {
	GsOverviewPage *page;  /* (unowned) */
	GsPluginJobListCategories *job;  /* (owned) */
} GetCategoriesData;

static void
get_categories_data_free (GetCategoriesData *data)
{
	g_clear_object (&data->job);
	g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GetCategoriesData, get_categories_data_free)

static void
gs_overview_page_get_categories_cb (GObject *source_object,
                                    GAsyncResult *res,
                                    gpointer user_data)
{
	g_autoptr(GetCategoriesData) data = g_steal_pointer (&user_data);
	GsOverviewPage *self = GS_OVERVIEW_PAGE (data->page);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	guint i;
	GsCategory *cat;
	GtkFlowBox *flowbox;
	GtkWidget *tile;
	guint added_cnt = 0;
	g_autoptr(GError) error = NULL;
	GPtrArray *list = NULL;  /* (element-type GsCategory) */

	if (!gs_plugin_loader_job_action_finish (plugin_loader, res, &error)) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get categories: %s", error->message);
		goto out;
	}

	list = gs_plugin_job_list_categories_get_result_list (data->job);

	gs_widget_remove_all (self->flowbox_categories, (GsRemoveFunc) gtk_flow_box_remove);
	gs_widget_remove_all (self->flowbox_iconless_categories, (GsRemoveFunc) gtk_flow_box_remove);

	/* Add categories to the flowboxes. Categories with icons are deemed to
	 * be visually important, and are listed near the top of the page.
	 * Categories without icons are listed in a separate flowbox at the
	 * bottom of the page. Typically they are addons. */
	for (i = 0; i < list->len; i++) {
		cat = GS_CATEGORY (g_ptr_array_index (list, i));
		if (gs_category_get_size (cat) == 0)
			continue;
		tile = gs_category_tile_new (cat);
		g_signal_connect (tile, "clicked",
				  G_CALLBACK (category_tile_clicked), self);

		if (gs_category_get_icon_name (cat) != NULL)
			flowbox = GTK_FLOW_BOX (self->flowbox_categories);
		else
			flowbox = GTK_FLOW_BOX (self->flowbox_iconless_categories);

		gtk_flow_box_insert (flowbox, tile, -1);
		gtk_widget_set_can_focus (gtk_widget_get_parent (tile), FALSE);
		added_cnt++;

		/* we save these for the 'More...' buttons */
		g_hash_table_insert (self->category_hash,
				     g_strdup (gs_category_get_id (cat)),
				     g_object_ref (cat));
	}

out:
	/* Show the heading for the iconless categories iff there are any. */
	gtk_widget_set_visible (self->iconless_categories_heading,
				gtk_flow_box_get_child_at_index (GTK_FLOW_BOX (self->flowbox_iconless_categories), 0) != NULL);

	if (added_cnt > 0)
		self->empty = FALSE;

	gs_overview_page_decrement_action_cnt (self);
}

static void
refresh_third_party_repo (GsOverviewPage *self)
{
	gtk_widget_set_visible (self->infobar_third_party, self->third_party_needs_question);
}

static gboolean
is_fedora (void)
{
	const gchar *id = NULL;
	g_autoptr(GsOsRelease) os_release = NULL;

	os_release = gs_os_release_new (NULL);
	if (os_release == NULL)
		return FALSE;

	id = gs_os_release_get_id (os_release);
	if (g_strcmp0 (id, "fedora") == 0)
		return TRUE;

	return FALSE;
}

static void
fedora_third_party_query_done_cb (GObject *source_object,
				  GAsyncResult *result,
				  gpointer user_data)
{
	GsFedoraThirdPartyState state = GS_FEDORA_THIRD_PARTY_STATE_UNKNOWN;
	g_autoptr(GsOverviewPage) self = user_data;
	g_autoptr(GError) error = NULL;

	if (!gs_fedora_third_party_query_finish (GS_FEDORA_THIRD_PARTY (source_object), result, &state, &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			return;
		g_warning ("Failed to query 'fedora-third-party': %s", error->message);
	} else {
		self->third_party_needs_question = state == GS_FEDORA_THIRD_PARTY_STATE_ASK;
	}

	refresh_third_party_repo (self);
}

static void
reload_third_party_repo (GsOverviewPage *self)
{
	/* Fedora-specific functionality */
	if (!is_fedora ())
		return;

	if (!gs_fedora_third_party_is_available (self->third_party))
		return;

	gs_fedora_third_party_query (self->third_party, self->cancellable, fedora_third_party_query_done_cb, g_object_ref (self));
}

static void
fedora_third_party_enable_done_cb (GObject *source_object,
				   GAsyncResult *result,
				   gpointer user_data)
{
	g_autoptr(GsOverviewPage) self = user_data;
	g_autoptr(GError) error = NULL;

	if (!gs_fedora_third_party_switch_finish (GS_FEDORA_THIRD_PARTY (source_object), result, &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			return;
		g_warning ("Failed to enable 'fedora-third-party': %s", error->message);
	}

	refresh_third_party_repo (self);
}

static void
fedora_third_party_enable (GsOverviewPage *self)
{
	gs_fedora_third_party_switch (self->third_party, TRUE, FALSE, self->cancellable, fedora_third_party_enable_done_cb, g_object_ref (self));
}

static void
fedora_third_party_disable_done_cb (GObject *source_object,
				    GAsyncResult *result,
				    gpointer user_data)
{
	g_autoptr(GsOverviewPage) self = user_data;
	g_autoptr(GError) error = NULL;

	if (!gs_fedora_third_party_opt_out_finish (GS_FEDORA_THIRD_PARTY (source_object), result, &error)) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			return;
		g_warning ("Failed to disable 'fedora-third-party': %s", error->message);
	}

	refresh_third_party_repo (self);
}

static void
fedora_third_party_disable (GsOverviewPage *self)
{
	gs_fedora_third_party_opt_out (self->third_party, self->cancellable, fedora_third_party_disable_done_cb, g_object_ref (self));
}

static gchar *
gs_overview_page_dup_deployment_featured_filename (void)
{
	g_autofree gchar *filename = NULL;
	const gchar * const *sys_dirs;

	#define FILENAME "deployment-featured.ini"

	filename = g_build_filename (SYSCONFDIR, "gnome-software", FILENAME, NULL);
	if (g_file_test (filename, G_FILE_TEST_IS_REGULAR)) {
		g_debug ("Found '%s'", filename);
		return g_steal_pointer (&filename);
	}
	g_debug ("File '%s' does not exist, trying next", filename);
	g_clear_pointer (&filename, g_free);

	sys_dirs = g_get_system_config_dirs ();

	for (guint i = 0; sys_dirs != NULL && sys_dirs[i]; i++) {
		g_autofree gchar *tmp = g_build_filename (sys_dirs[i], "gnome-software", FILENAME, NULL);
		if (g_file_test (tmp, G_FILE_TEST_IS_REGULAR)) {
			g_debug ("Found '%s'", tmp);
			return g_steal_pointer (&tmp);
		}
		g_debug ("File '%s' does not exist, trying next", tmp);
	}

	sys_dirs = g_get_system_data_dirs ();

	for (guint i = 0; sys_dirs != NULL && sys_dirs[i]; i++) {
		g_autofree gchar *tmp = g_build_filename (sys_dirs[i], "gnome-software", FILENAME, NULL);
		if (g_file_test (tmp, G_FILE_TEST_IS_REGULAR)) {
			g_debug ("Found '%s'", tmp);
			return g_steal_pointer (&tmp);
		}
		g_debug ("File '%s' does not exist, %s", tmp, sys_dirs[i + 1] ? "trying next" : "no more files to try");
	}

	#undef FILENAME

	return NULL;
}

static gboolean
gs_overview_page_read_deployment_featured_keys (gchar **out_label,
						gchar ***out_deployment_featured)
{
	g_autoptr(GKeyFile) key_file = NULL;
	g_autoptr(GPtrArray) array = NULL;
	g_auto(GStrv) selector = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *filename = NULL;

	filename = gs_overview_page_dup_deployment_featured_filename ();

	if (filename == NULL)
		return FALSE;

	key_file = g_key_file_new ();
	if (!g_key_file_load_from_file (key_file, filename, G_KEY_FILE_NONE, &error)) {
		g_debug ("Failed to read '%s': %s", filename, error->message);
		return FALSE;
	}

	*out_label = g_key_file_get_locale_string (key_file, "Deployment Featured Apps", "Title", NULL, NULL);

	if (*out_label == NULL || **out_label == '\0') {
		g_clear_pointer (out_label, g_free);
		return FALSE;
	}

	selector = g_key_file_get_string_list (key_file, "Deployment Featured Apps", "Selector", NULL, NULL);

	/* Sanitize the content */
	if (selector == NULL) {
		g_clear_pointer (out_label, g_free);
		return FALSE;
	}

	array = g_ptr_array_sized_new (g_strv_length (selector) + 1);

	for (guint i = 0; selector[i] != NULL; i++) {
		const gchar *value = g_strstrip (selector[i]);
		if (*value != '\0')
			g_ptr_array_add (array, g_strdup (value));
	}

	if (array->len == 0) {
		g_clear_pointer (out_label, g_free);
		return FALSE;
	}

	g_ptr_array_add (array, NULL);

	*out_deployment_featured = (gchar **) g_ptr_array_free (g_steal_pointer (&array), FALSE);

	return TRUE;
}

static void
gs_overview_page_load (GsOverviewPage *self)
{
	self->empty = TRUE;

	if (!self->loading_featured) {
		g_autoptr(GsPluginJob) plugin_job = NULL;
		g_autoptr(GsAppQuery) query = NULL;
		GsPluginListAppsFlags flags = GS_PLUGIN_LIST_APPS_FLAGS_INTERACTIVE;

		query = gs_app_query_new ("is-featured", GS_APP_QUERY_TRISTATE_TRUE,
					  "max-results", 5,
					  "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
					  "dedupe-flags", GS_APP_LIST_FILTER_FLAG_PREFER_INSTALLED |
							  GS_APP_LIST_FILTER_FLAG_KEY_ID_PROVIDES,
					  "filter-func", filter_hi_res_icon,
					  "filter-user-data", self,
					  NULL);

		plugin_job = gs_plugin_job_list_apps_new (query, flags);

		self->loading_featured = TRUE;
		gs_plugin_loader_job_process_async (self->plugin_loader,
						    plugin_job,
						    self->cancellable,
						    gs_overview_page_get_featured_cb,
						    self);
		self->action_cnt++;
	}

	if (!self->loading_deployment_featured && self->deployment_featured != NULL) {
		g_autoptr(GsPluginJob) plugin_job = NULL;
		g_autoptr(GsAppQuery) query = NULL;
		GsPluginListAppsFlags flags = GS_PLUGIN_LIST_APPS_FLAGS_INTERACTIVE;

		self->loading_deployment_featured = TRUE;

		query = gs_app_query_new ("deployment-featured", self->deployment_featured,
					  "max-results", N_TILES,
					  "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING |
							  GS_PLUGIN_REFINE_FLAGS_REQUIRE_CATEGORIES |
							  GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
					  "dedupe-flags", GS_APP_LIST_FILTER_FLAG_PREFER_INSTALLED |
							  GS_APP_LIST_FILTER_FLAG_KEY_ID_PROVIDES,
					  NULL);

		plugin_job = gs_plugin_job_list_apps_new (query, flags);

		gs_plugin_loader_job_process_async (self->plugin_loader,
						    plugin_job,
						    self->cancellable,
						    gs_overview_page_get_deployment_featured_cb,
						    self);
		self->action_cnt++;
	}

	if (!self->loading_curated) {
		g_autoptr(GsPluginJob) plugin_job = NULL;
		g_autoptr(GsAppQuery) query = NULL;
		GsPluginListAppsFlags flags = GS_PLUGIN_LIST_APPS_FLAGS_INTERACTIVE;

		query = gs_app_query_new ("is-curated", GS_APP_QUERY_TRISTATE_TRUE,
					  "max-results", N_TILES,
					  "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING |
							  GS_PLUGIN_REFINE_FLAGS_REQUIRE_CATEGORIES |
							  GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
					  "dedupe-flags", GS_APP_LIST_FILTER_FLAG_PREFER_INSTALLED |
							  GS_APP_LIST_FILTER_FLAG_KEY_ID_PROVIDES,
					  NULL);

		plugin_job = gs_plugin_job_list_apps_new (query, flags);

		self->loading_curated = TRUE;
		gs_plugin_loader_job_process_async (self->plugin_loader,
						    plugin_job,
						    self->cancellable,
						    gs_overview_page_get_curated_cb,
						    self);
		self->action_cnt++;
	}

	if (!self->loading_recent) {
		g_autoptr(GsPluginJob) plugin_job = NULL;
		g_autoptr(GDateTime) now = NULL;
		g_autoptr(GDateTime) released_since = NULL;
		g_autoptr(GsAppQuery) query = NULL;
		GsPluginListAppsFlags flags = GS_PLUGIN_LIST_APPS_FLAGS_INTERACTIVE;

		now = g_date_time_new_now_local ();
		released_since = g_date_time_add_seconds (now, -(60 * 60 * 24 * 30));
		query = gs_app_query_new ("released-since", released_since,
					  "max-results", N_TILES,
					  "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING |
							  GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
					  "dedupe-flags", GS_APP_LIST_FILTER_FLAG_KEY_ID |
							  GS_APP_LIST_FILTER_FLAG_PREFER_INSTALLED |
							  GS_APP_LIST_FILTER_FLAG_KEY_ID_PROVIDES,
					  "sort-func", gs_overview_page_sort_recent_cb,
					  "filter-func", gs_overview_page_filter_recent_cb,
					  NULL);

		plugin_job = gs_plugin_job_list_apps_new (query, flags);

		self->loading_recent = TRUE;
		gs_plugin_loader_job_process_async (self->plugin_loader,
						    plugin_job,
						    self->cancellable,
						    gs_overview_page_get_recent_cb,
						    self);
		self->action_cnt++;
	}

	if (!self->loading_categories) {
		g_autoptr(GsPluginJob) plugin_job = NULL;
		GsPluginRefineCategoriesFlags flags = GS_PLUGIN_REFINE_CATEGORIES_FLAGS_INTERACTIVE |
		                                      GS_PLUGIN_REFINE_CATEGORIES_FLAGS_SIZE;
		g_autoptr(GetCategoriesData) data = NULL;

		self->loading_categories = TRUE;
		plugin_job = gs_plugin_job_list_categories_new (flags);

		data = g_new0 (GetCategoriesData, 1);
		data->page = self;
		data->job = g_object_ref (GS_PLUGIN_JOB_LIST_CATEGORIES (plugin_job));

		gs_plugin_loader_job_process_async (self->plugin_loader, plugin_job,
						    self->cancellable, gs_overview_page_get_categories_cb,
						    g_steal_pointer (&data));
		self->action_cnt++;
	}

	reload_third_party_repo (self);
}

static void
gs_overview_page_reload (GsPage *page)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (page);
	self->featured_overwritten = FALSE;
	gs_overview_page_invalidate (self);
	gs_overview_page_load (self);
}

static void
gs_overview_page_switch_to (GsPage *page)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (page);

	if (gs_shell_get_mode (self->shell) != GS_SHELL_MODE_OVERVIEW) {
		g_warning ("Called switch_to(overview) when in mode %s",
			   gs_shell_get_mode_string (self->shell));
		return;
	}

	gs_grab_focus_when_mapped (self->scrolledwindow_overview);

	if (self->cache_valid || self->action_cnt > 0)
		return;
	gs_overview_page_load (self);
}

static void
gs_overview_page_refresh_cb (GsPluginLoader *plugin_loader,
			     GAsyncResult *result,
			     GsOverviewPage *self)
{
	gboolean success;
	g_autoptr(GError) error = NULL;

	success = gs_plugin_loader_job_action_finish (plugin_loader, result, &error);
	if (!success &&
	    !g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED) &&
	    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_warning ("failed to refresh: %s", error->message);

	if (success)
		g_signal_emit_by_name (self->plugin_loader, "reload", 0, NULL);
}

static void
third_party_response_cb (GtkInfoBar *info_bar,
                         gint response_id,
                         GsOverviewPage *self)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;

	if (response_id == GTK_RESPONSE_YES)
		fedora_third_party_enable (self);
	else
		fedora_third_party_disable (self);

	self->third_party_needs_question = FALSE;
	refresh_third_party_repo (self);

	plugin_job = gs_plugin_job_refresh_metadata_new (1,
							 GS_PLUGIN_REFRESH_METADATA_FLAGS_NONE);
	gs_plugin_loader_job_process_async (self->plugin_loader, plugin_job,
					    self->cancellable,
					    (GAsyncReadyCallback) gs_overview_page_refresh_cb,
					    self);
}

static gboolean
gs_overview_page_setup (GsPage *page,
                        GsShell *shell,
                        GsPluginLoader *plugin_loader,
                        GCancellable *cancellable,
                        GError **error)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (page);
	GtkWidget *tile;
	gint i;
	g_autofree gchar *text = NULL;
	g_autofree gchar *link = NULL;

	g_return_val_if_fail (GS_IS_OVERVIEW_PAGE (self), TRUE);

	self->plugin_loader = g_object_ref (plugin_loader);
	self->third_party = gs_fedora_third_party_new ();
	self->cancellable = g_object_ref (cancellable);
	self->category_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
						     g_free, (GDestroyNotify) g_object_unref);

	link = g_strdup_printf ("<a href=\"%s\">%s</a>",
	                        "https://docs.fedoraproject.org/en-US/workstation-working-group/third-party-repos/",
	                        /* Translators: This is a clickable link on the third party repositories info bar. It's
				   part of a constructed sentence: "Provides access to additional software from [selected external sources].
				   Some proprietary software is included." */
	                        _("selected external sources"));
	/* Translators: This is the third party repositories info bar. The %s is replaced with "selected external sources" link. */
	text = g_strdup_printf (_("Provides access to additional software from %s. Some proprietary software is included."),
				link);
	gtk_label_set_markup (GTK_LABEL (self->label_third_party), text);

	/* create info bar if not already dismissed in initial-setup */
	refresh_third_party_repo (self);
	reload_third_party_repo (self);
	gtk_info_bar_add_button (GTK_INFO_BAR (self->infobar_third_party),
				 /* TRANSLATORS: button to turn on third party software repositories */
				 _("Enable"), GTK_RESPONSE_YES);
	g_signal_connect (self->infobar_third_party, "response",
			  G_CALLBACK (third_party_response_cb), self);

	/* avoid a ref cycle */
	self->shell = shell;

	for (i = 0; i < N_TILES; i++) {
		tile = gs_summary_tile_new (NULL);
		gtk_flow_box_insert (GTK_FLOW_BOX (self->box_curated), tile, -1);
	}

	for (i = 0; i < N_TILES; i++) {
		tile = gs_summary_tile_new (NULL);
		gtk_flow_box_insert (GTK_FLOW_BOX (self->box_recent), tile, -1);
	}

	return TRUE;
}

static void
refreshed_cb (GsOverviewPage *self, gpointer user_data)
{
	if (self->empty) {
		gtk_stack_set_visible_child_name (GTK_STACK (self->stack_overview), "no-results");
	} else {
		gtk_stack_set_visible_child_name (GTK_STACK (self->stack_overview), "overview");
	}
}

static void
gs_overview_page_init (GsOverviewPage *self)
{
	g_autofree gchar *tmp_label = NULL;

	gtk_widget_init_template (GTK_WIDGET (self));

	gs_featured_carousel_set_apps (GS_FEATURED_CAROUSEL (self->featured_carousel), NULL);

	g_signal_connect (self, "refreshed", G_CALLBACK (refreshed_cb), self);

	if (gs_overview_page_read_deployment_featured_keys (&tmp_label, &self->deployment_featured))
		gtk_label_set_text (GTK_LABEL (self->deployment_featured_heading), tmp_label);
}

static void
gs_overview_page_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (object);

	switch ((GsOverviewPageProperty) prop_id) {
	case PROP_VADJUSTMENT:
		g_value_set_object (value, gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolledwindow_overview)));
		break;
	case PROP_TITLE:
		/* Translators: This is the title of the main page of the UI. */
		g_value_set_string (value, _("Explore"));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gs_overview_page_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
	switch ((GsOverviewPageProperty) prop_id) {
	case PROP_VADJUSTMENT:
	case PROP_TITLE:
		/* Read only. */
		g_assert_not_reached ();
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gs_overview_page_dispose (GObject *object)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (object);

	g_clear_object (&self->plugin_loader);
	g_clear_object (&self->cancellable);
	g_clear_object (&self->third_party);
	g_clear_pointer (&self->category_hash, g_hash_table_unref);
	g_clear_pointer (&self->deployment_featured, g_strfreev);

	G_OBJECT_CLASS (gs_overview_page_parent_class)->dispose (object);
}

static void
gs_overview_page_class_init (GsOverviewPageClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPageClass *page_class = GS_PAGE_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->get_property = gs_overview_page_get_property;
	object_class->set_property = gs_overview_page_set_property;
	object_class->dispose = gs_overview_page_dispose;

	page_class->switch_to = gs_overview_page_switch_to;
	page_class->reload = gs_overview_page_reload;
	page_class->setup = gs_overview_page_setup;

	g_object_class_override_property (object_class, PROP_VADJUSTMENT, "vadjustment");
	g_object_class_override_property (object_class, PROP_TITLE, "title");

	signals [SIGNAL_REFRESHED] =
		g_signal_new ("refreshed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-overview-page.ui");

	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, infobar_third_party);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, label_third_party);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, featured_carousel);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, box_overview);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, box_curated);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, box_recent);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, box_deployment_featured);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, flowbox_categories);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, flowbox_iconless_categories);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, iconless_categories_heading);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, curated_heading);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, recent_heading);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, deployment_featured_heading);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, scrolledwindow_overview);
	gtk_widget_class_bind_template_child (widget_class, GsOverviewPage, stack_overview);
	gtk_widget_class_bind_template_callback (widget_class, featured_carousel_app_clicked_cb);
}

GsOverviewPage *
gs_overview_page_new (void)
{
	return GS_OVERVIEW_PAGE (g_object_new (GS_TYPE_OVERVIEW_PAGE, NULL));
}

void
gs_overview_page_override_featured (GsOverviewPage	*self,
				    GsApp		*app)
{
	g_autoptr(GsAppList) list = NULL;

	g_return_if_fail (GS_IS_OVERVIEW_PAGE (self));
	g_return_if_fail (GS_IS_APP (app));

	self->featured_overwritten = TRUE;

	list = gs_app_list_new ();
	gs_app_list_add (list, app);
	gs_featured_carousel_set_apps (GS_FEATURED_CAROUSEL (self->featured_carousel), list);
}
