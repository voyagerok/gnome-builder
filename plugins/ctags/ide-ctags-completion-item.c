/* ide-ctags-completion-item.c
 *
 * Copyright (C) 2015 Christian Hergert <christian@hergert.me>
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
 */

#define G_LOG_DOMAIN "ide-ctags-completion-item"

#include <glib/gi18n.h>

#include "egg-counter.h"

#include "ide-ctags-completion-item.h"
#include "ide-ctags-index.h"

EGG_DEFINE_COUNTER (instances, "IdeCtagsCompletionItem", "Instances",
                    "Number of IdeCtagsCompletionItems")

struct _IdeCtagsCompletionItem
{
  GObject                     parent_instance;
  const IdeCtagsIndexEntry   *entry;
  IdeCtagsCompletionProvider *provider;
  GtkSourceCompletionContext *context;
};

static void proposal_iface_init (GtkSourceCompletionProposalIface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (IdeCtagsCompletionItem,
                                ide_ctags_completion_item,
                                G_TYPE_OBJECT,
                                0,
                                G_IMPLEMENT_INTERFACE (GTK_SOURCE_TYPE_COMPLETION_PROPOSAL,
                                                       proposal_iface_init))

GtkSourceCompletionProposal *
ide_ctags_completion_item_new (const IdeCtagsIndexEntry   *entry,
                               IdeCtagsCompletionProvider *provider,
                               GtkSourceCompletionContext *context)
{
  IdeCtagsCompletionItem *self;

  self= g_object_new (IDE_TYPE_CTAGS_COMPLETION_ITEM, NULL);

  /*
   * use borrowed references to avoid the massive amount of reference counting.
   * we don't need them since we know the provider will outlast us.
   */
  self->entry = entry;
  self->provider = provider;

  /*
   * There is the slight chance the context will get disposed out of
   * our control. I've seen this happen a few times now.
   */
  ide_set_weak_pointer (&self->context, context);

  return GTK_SOURCE_COMPLETION_PROPOSAL (self);
}

gint
ide_ctags_completion_item_compare (IdeCtagsCompletionItem *itema,
                                   IdeCtagsCompletionItem *itemb)
{
  return ide_ctags_index_entry_compare (itema->entry, itemb->entry);
}

static void
ide_ctags_completion_item_finalize (GObject *object)
{
  IdeCtagsCompletionItem *self = (IdeCtagsCompletionItem *)object;

  ide_clear_weak_pointer (&self->context);

  G_OBJECT_CLASS (ide_ctags_completion_item_parent_class)->finalize (object);

  EGG_COUNTER_DEC (instances);
}

static void
ide_ctags_completion_item_class_init (IdeCtagsCompletionItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ide_ctags_completion_item_finalize;
}

static void
ide_ctags_completion_item_class_finalize (IdeCtagsCompletionItemClass *klass)
{
}

static void
ide_ctags_completion_item_init (IdeCtagsCompletionItem *self)
{
  EGG_COUNTER_INC (instances);
}

static gchar *
get_label (GtkSourceCompletionProposal *proposal)
{
  IdeCtagsCompletionItem *self = (IdeCtagsCompletionItem *)proposal;

  return g_strdup (self->entry->name);
}

static gchar *
get_text (GtkSourceCompletionProposal *proposal)
{
  IdeCtagsCompletionItem *self = (IdeCtagsCompletionItem *)proposal;

  return g_strdup (self->entry->name);
}

static const gchar *
get_icon_name (GtkSourceCompletionProposal *proposal)
{
  IdeCtagsCompletionItem *self = (IdeCtagsCompletionItem *)proposal;
  const gchar *icon_name = NULL;

  if (self->entry == NULL)
    return NULL;

  switch (self->entry->kind)
    {
    case IDE_CTAGS_INDEX_ENTRY_CLASS_NAME:
      icon_name = "lang-clang-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_ENUMERATOR:
      icon_name = "lang-enum-value-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_ENUMERATION_NAME:
      icon_name = "lang-enum-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_PROTOTYPE:
    case IDE_CTAGS_INDEX_ENTRY_FUNCTION:
      icon_name = "lang-function-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_FILE_NAME:
      icon_name = "text-x-generic-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_MEMBER:
      icon_name = "lang-struct-field-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_UNION:
      icon_name = "lang-union-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_TYPEDEF:
      icon_name = "lang-typedef-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_STRUCTURE:
      icon_name = "lang-struct-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_VARIABLE:
      icon_name = "lang-variable-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_DEFINE:
      icon_name = "lang-define-symbolic";
      break;

    case IDE_CTAGS_INDEX_ENTRY_ANCHOR:
    default:
      return NULL;
    }

  return icon_name;
}

static void
proposal_iface_init (GtkSourceCompletionProposalIface *iface)
{
  iface->get_label = get_label;
  iface->get_text = get_text;
  iface->get_icon_name = get_icon_name;
}

void
_ide_ctags_completion_item_register_type (GTypeModule *module)
{
  ide_ctags_completion_item_register_type (module);
}