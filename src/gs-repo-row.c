/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2015-2018 Kalev Lember <klember@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "config.h"

#include <glib/gi18n.h>
#include <libsoup/soup.h>

#include "gs-repo-row.h"

typedef struct
{
	GsPluginLoader	*plugin_loader; /* owned */
	GsApp		*repo;
	GtkWidget	*name_label;
	GtkWidget	*hostname_label;
	GtkWidget	*comment_label;
	GtkWidget	*remove_button;
	GtkWidget	*disable_switch;
	gulong		 switch_handler_id;
	guint		 refresh_idle_id;
	guint		 busy_counter;
	gboolean	 supports_remove;
	gboolean	 supports_enable_disable;
} GsRepoRowPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GsRepoRow, gs_repo_row, GTK_TYPE_LIST_BOX_ROW)

enum {
	SIGNAL_REMOVE_CLICKED,
	SIGNAL_SWITCH_CLICKED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

static void
refresh_ui (GsRepoRow *row)
{
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (row);
	gboolean active = FALSE;
	gboolean state_sensitive = FALSE;
	gboolean busy = priv->busy_counter> 0;
	gboolean is_system_repo;

	if (priv->repo == NULL) {
		gtk_widget_set_sensitive (priv->disable_switch, FALSE);
		gtk_switch_set_active (GTK_SWITCH (priv->disable_switch), FALSE);
		return;
	}

	g_signal_handler_block (priv->disable_switch, priv->switch_handler_id);
	gtk_widget_set_sensitive (priv->disable_switch, TRUE);

	switch (gs_app_get_state (priv->repo)) {
	case GS_APP_STATE_AVAILABLE:
	case GS_APP_STATE_AVAILABLE_LOCAL:
		active = FALSE;
		state_sensitive = TRUE;
		break;
	case GS_APP_STATE_INSTALLED:
		active = TRUE;
		break;
	case GS_APP_STATE_INSTALLING:
		active = TRUE;
		busy = TRUE;
		break;
	case GS_APP_STATE_REMOVING:
		active = FALSE;
		busy = TRUE;
		break;
	case GS_APP_STATE_UNAVAILABLE:
		g_signal_handler_unblock (priv->disable_switch, priv->switch_handler_id);
		gtk_widget_destroy (GTK_WIDGET (row));
		return;
	default:
		state_sensitive = TRUE;
		break;
	}

	is_system_repo = gs_app_has_quirk (priv->repo, GS_APP_QUIRK_PROVENANCE);

	/* Disable for the system repos, if installed */
	gtk_widget_set_sensitive (priv->disable_switch, priv->supports_enable_disable && (state_sensitive || !is_system_repo));
	gtk_widget_set_visible (priv->remove_button, priv->supports_remove && !is_system_repo);

	/* Set only the 'state' to visually indicate the state is not saved yet */
	if (busy)
		gtk_switch_set_state (GTK_SWITCH (priv->disable_switch), active);
	else
		gtk_switch_set_active (GTK_SWITCH (priv->disable_switch), active);

	g_signal_handler_unblock (priv->disable_switch, priv->switch_handler_id);
}

static gboolean
refresh_idle (gpointer user_data)
{
	g_autoptr(GsRepoRow) row = (GsRepoRow *) user_data;
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (row);

	priv->refresh_idle_id = 0;

	refresh_ui (row);

	return G_SOURCE_REMOVE;
}

static void
repo_state_changed_cb (GsApp *repo, GParamSpec *pspec, GsRepoRow *row)
{
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (row);

	if (priv->refresh_idle_id > 0)
		return;
	priv->refresh_idle_id = g_idle_add (refresh_idle, g_object_ref (row));
}

static gchar *
get_repo_installed_text (GsApp *repo)
{
	GsAppList *related;
	guint cnt_addon = 0;
	guint cnt_apps = 0;
	g_autofree gchar *addons_text = NULL;
	g_autofree gchar *apps_text = NULL;

	related = gs_app_get_related (repo);
	for (guint i = 0; i < gs_app_list_length (related); i++) {
		GsApp *app_tmp = gs_app_list_index (related, i);
		switch (gs_app_get_kind (app_tmp)) {
		case AS_COMPONENT_KIND_WEB_APP:
		case AS_COMPONENT_KIND_DESKTOP_APP:
			cnt_apps++;
			break;
		case AS_COMPONENT_KIND_FONT:
		case AS_COMPONENT_KIND_CODEC:
		case AS_COMPONENT_KIND_INPUT_METHOD:
		case AS_COMPONENT_KIND_ADDON:
			cnt_addon++;
			break;
		default:
			break;
		}
	}

	if (cnt_addon == 0) {
		/* TRANSLATORS: This string is used to construct the 'X applications
		   installed' sentence, describing a software repository. */
		return g_strdup_printf (ngettext ("%u application installed",
		                                  "%u applications installed",
		                                  cnt_apps), cnt_apps);
	}
	if (cnt_apps == 0) {
		/* TRANSLATORS: This string is used to construct the 'X add-ons
		   installed' sentence, describing a software repository. */
		return g_strdup_printf (ngettext ("%u add-on installed",
		                                  "%u add-ons installed",
		                                  cnt_addon), cnt_addon);
	}

	/* TRANSLATORS: This string is used to construct the 'X applications
	   and y add-ons installed' sentence, describing a software repository.
	   The correct form here depends on the number of applications. */
	apps_text = g_strdup_printf (ngettext ("%u application",
	                                       "%u applications",
	                                       cnt_apps), cnt_apps);
	/* TRANSLATORS: This string is used to construct the 'X applications
	   and y add-ons installed' sentence, describing a software repository.
	   The correct form here depends on the number of add-ons. */
	addons_text = g_strdup_printf (ngettext ("%u add-on",
	                                         "%u add-ons",
	                                         cnt_addon), cnt_addon);
	/* TRANSLATORS: This string is used to construct the 'X applications
	   and y add-ons installed' sentence, describing a software repository.
	   The correct form here depends on the total number of
	   applications and add-ons. */
	return g_strdup_printf (ngettext ("%s and %s installed",
	                                  "%s and %s installed",
	                                  cnt_apps + cnt_addon),
	                                  apps_text, addons_text);
}

static void
gs_repo_row_set_repo (GsRepoRow *self, GsApp *repo)
{
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (self);
	GsPlugin *plugin;
	g_autofree gchar *comment = NULL;
	const gchar *tmp;

	g_assert (priv->repo == NULL);

	priv->repo = g_object_ref (repo);
	g_signal_connect_object (priv->repo, "notify::state",
	                         G_CALLBACK (repo_state_changed_cb),
	                         self, 0);

	plugin = gs_plugin_loader_find_plugin (priv->plugin_loader, gs_app_get_management_plugin (repo));
	priv->supports_remove = plugin != NULL && gs_plugin_get_action_supported (plugin, GS_PLUGIN_ACTION_REMOVE_REPO);
	priv->supports_enable_disable = plugin != NULL &&
		gs_plugin_get_action_supported (plugin, GS_PLUGIN_ACTION_ENABLE_REPO) &&
		gs_plugin_get_action_supported (plugin, GS_PLUGIN_ACTION_DISABLE_REPO);

	gtk_label_set_label (GTK_LABEL (priv->name_label), gs_app_get_name (repo));

	gtk_widget_set_visible (priv->hostname_label, FALSE);

	tmp = gs_app_get_url (repo, AS_URL_KIND_HOMEPAGE);
	if (tmp != NULL && *tmp != '\0') {
		g_autoptr(SoupURI) uri = NULL;

		uri = soup_uri_new (tmp);
		if (uri && soup_uri_get_host (uri) != NULL && *soup_uri_get_host (uri) != '\0') {
			gtk_label_set_label (GTK_LABEL (priv->hostname_label), soup_uri_get_host (uri));
			gtk_widget_set_visible (priv->hostname_label, TRUE);
		}
	}

	comment = get_repo_installed_text (repo);
	tmp = gs_app_get_metadata_item (priv->repo, "GnomeSoftware::InstallationKind");
	if (tmp != NULL && *tmp != '\0') {
		gchar *cnt;

		/* Translators: The first '%s' is replaced with a text like '10 applications installed',
		      the second '%s' is replaced with installation kind, like in case of Flatpak 'User Installation'. */
		cnt = g_strdup_printf (C_("repo-row", "%s • %s"), comment, tmp);
		g_clear_pointer (&comment, g_free);
		comment = cnt;
	}

	gtk_label_set_label (GTK_LABEL (priv->comment_label), comment);

	refresh_ui (self);
}

GsApp *
gs_repo_row_get_repo (GsRepoRow *self)
{
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (self);
	g_return_val_if_fail (GS_IS_REPO_ROW (self), NULL);
	return priv->repo;
}

static void
disable_switch_clicked_cb (GtkWidget *widget,
			   GParamSpec *param,
			   GsRepoRow *row)
{
	g_return_if_fail (GS_IS_REPO_ROW (row));
	gs_repo_row_emit_switch_clicked (row);
}

static void
gs_repo_row_remove_button_clicked_cb (GtkWidget *button,
				      GsRepoRow *row)
{
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (row);

	g_return_if_fail (GS_IS_REPO_ROW (row));

	if (priv->repo == NULL || priv->busy_counter)
		return;

	g_signal_emit (row, signals[SIGNAL_REMOVE_CLICKED], 0);
}

static void
gs_repo_row_destroy (GtkWidget *object)
{
	GsRepoRow *self = GS_REPO_ROW (object);
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (self);

	if (priv->repo != NULL) {
		g_signal_handlers_disconnect_by_func (priv->repo, repo_state_changed_cb, self);
		g_clear_object (&priv->repo);
	}

	if (priv->refresh_idle_id != 0) {
		g_source_remove (priv->refresh_idle_id);
		priv->refresh_idle_id = 0;
	}

	g_clear_object (&priv->plugin_loader);

	GTK_WIDGET_CLASS (gs_repo_row_parent_class)->destroy (object);
}

static void
gs_repo_row_init (GsRepoRow *self)
{
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (self);
	GtkWidget *image;

	gtk_widget_init_template (GTK_WIDGET (self));
	priv->switch_handler_id = g_signal_connect (priv->disable_switch, "notify::active",
						    G_CALLBACK (disable_switch_clicked_cb), self);
	image = gtk_image_new_from_icon_name ("user-trash-symbolic", GTK_ICON_SIZE_BUTTON);
	gtk_button_set_image (GTK_BUTTON (priv->remove_button), image);
	g_signal_connect (priv->remove_button, "clicked",
		G_CALLBACK (gs_repo_row_remove_button_clicked_cb), self);
}

static void
gs_repo_row_class_init (GsRepoRowClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	widget_class->destroy = gs_repo_row_destroy;

	signals [SIGNAL_REMOVE_CLICKED] =
		g_signal_new ("remove-clicked",
		              G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (GsRepoRowClass, remove_clicked),
		              NULL, NULL, g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0, G_TYPE_NONE);

	signals [SIGNAL_SWITCH_CLICKED] =
		g_signal_new ("switch-clicked",
		              G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (GsRepoRowClass, switch_clicked),
		              NULL, NULL, g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0, G_TYPE_NONE);

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-repo-row.ui");

	gtk_widget_class_bind_template_child_private (widget_class, GsRepoRow, name_label);
	gtk_widget_class_bind_template_child_private (widget_class, GsRepoRow, hostname_label);
	gtk_widget_class_bind_template_child_private (widget_class, GsRepoRow, comment_label);
	gtk_widget_class_bind_template_child_private (widget_class, GsRepoRow, remove_button);
	gtk_widget_class_bind_template_child_private (widget_class, GsRepoRow, disable_switch);
}

GtkWidget *
gs_repo_row_new (GsPluginLoader	*plugin_loader,
		 GsApp *repo)
{
	GsRepoRow *row = g_object_new (GS_TYPE_REPO_ROW, NULL);
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (row);
	priv->plugin_loader = g_object_ref (plugin_loader);
	gs_repo_row_set_repo (row, repo);
	return GTK_WIDGET (row);
}

static void
gs_repo_row_change_busy (GsRepoRow *self,
			 gboolean value)
{
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (self);

	g_return_if_fail (GS_IS_REPO_ROW (self));

	if (value)
		g_return_if_fail (priv->busy_counter + 1 > priv->busy_counter);
	else
		g_return_if_fail (priv->busy_counter > 0);

	priv->busy_counter += (value ? 1 : -1);

	if (value && priv->busy_counter == 1)
		gtk_widget_set_sensitive (priv->disable_switch, FALSE);
	else if (!value && !priv->busy_counter)
		refresh_ui (self);
}

/**
 * gs_repo_row_mark_busy:
 * @row: a #GsRepoRow
 *
 * Mark the @row as busy, that is the @row has pending operation(s).
 * Unmark the @row as busy with gs_repo_row_unmark_busy() once
 * the operation is done. This can be called mutliple times, only call
 * the gs_repo_row_unmark_busy() as many times as this function had
 * been called.
 *
 * Since: 41
 **/
void
gs_repo_row_mark_busy (GsRepoRow *row)
{
	gs_repo_row_change_busy (row, TRUE);
}

/**
 * gs_repo_row_unmark_busy:
 * @row: a #GsRepoRow
 *
 * A pair function for gs_repo_row_mark_busy().
 *
 * Since: 41
 **/
void
gs_repo_row_unmark_busy (GsRepoRow *row)
{
	gs_repo_row_change_busy (row, FALSE);
}

/**
 * gs_repo_row_get_is_busy:
 * @row: a #GsRepoRow
 *
 * Returns: %TRUE, when there is any pending operation for the @row
 *
 * Since: 41
 **/
gboolean
gs_repo_row_get_is_busy (GsRepoRow *row)
{
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (row);

	g_return_val_if_fail (GS_IS_REPO_ROW (row), FALSE);

	return priv->busy_counter > 0;
}

/**
 * gs_repo_row_emit_switch_clicked:
 * @self: a #GsRepoRow
 *
 * Emits the GsRepoRow:switch-clicked signal, if applicable.
 *
 * Since: 41
 **/
void
gs_repo_row_emit_switch_clicked (GsRepoRow *self)
{
	GsRepoRowPrivate *priv = gs_repo_row_get_instance_private (self);

	g_return_if_fail (GS_IS_REPO_ROW (self));

	if (priv->repo == NULL || priv->busy_counter > 0 || !gtk_widget_get_visible (priv->disable_switch))
		return;

	g_signal_emit (self, signals[SIGNAL_SWITCH_CLICKED], 0);
}
