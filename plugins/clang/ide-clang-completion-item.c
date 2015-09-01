/* ide-clang-completion-item.c
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

#define G_LOG_DOMAIN "ide-clang-completion"

#include <clang-c/Index.h>
#include <glib/gi18n.h>

#include "ide-clang-completion-item.h"
#include "ide-debug.h"
#include "ide-ref-ptr.h"
#include "ide-source-snippet.h"
#include "ide-source-snippet-chunk.h"


struct _IdeClangCompletionItem
{
  GObject           parent_instance;

  guint             index;
  gint              typed_text_index;
  guint             initialized : 1;

  const gchar      *icon_name;
  gchar            *brief_comment;
  gchar            *markup;
  IdeRefPtr        *results;
  IdeSourceSnippet *snippet;
  gchar            *typed_text;
};

static void completion_proposal_iface_init (GtkSourceCompletionProposalIface *);

G_DEFINE_TYPE_WITH_CODE (IdeClangCompletionItem, ide_clang_completion_item, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (GTK_SOURCE_TYPE_COMPLETION_PROPOSAL,
                                                completion_proposal_iface_init))

enum {
  PROP_0,
  PROP_INDEX,
  PROP_RESULTS,
  LAST_PROP
};

static GParamSpec *gParamSpecs [LAST_PROP];

static CXCompletionResult *
ide_clang_completion_item_get_result (IdeClangCompletionItem *self)
{
  CXCodeCompleteResults *results;

  g_assert (IDE_IS_CLANG_COMPLETION_ITEM (self));

  results = ide_ref_ptr_get (self->results);
  return &results->Results [self->index];
}

static void
ide_clang_completion_item_lazy_init (IdeClangCompletionItem *self)
{
  CXCompletionResult *result;
  g_autoptr(IdeSourceSnippet) snippet = NULL;
  GString *markup = NULL;
  unsigned num_chunks;
  unsigned i;
  guint tab_stop = 0;

  g_assert (IDE_IS_CLANG_COMPLETION_ITEM (self));

  if (G_LIKELY (self->initialized))
    return;

  result = ide_clang_completion_item_get_result (self);
  num_chunks = clang_getNumCompletionChunks (result);
  snippet = ide_source_snippet_new (NULL, NULL);
  markup = g_string_new (NULL);

  g_assert (result);
  g_assert (num_chunks);
  g_assert (IDE_IS_SOURCE_SNIPPET (snippet));
  g_assert (markup);

  /*
   * Try to determine the icon to use for this result.
   */
  switch ((int)result->CursorKind)
    {
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_MemberRef:
    case CXCursor_MemberRefExpr:
    case CXCursor_ObjCClassMethodDecl:
    case CXCursor_ObjCInstanceMethodDecl:
      self->icon_name = "lang-method-symbolic";
      break;

    case CXCursor_ConversionFunction:
    case CXCursor_FunctionDecl:
    case CXCursor_FunctionTemplate:
      self->icon_name = "lang-function-symbolic";
      break;

    case CXCursor_FieldDecl:
      self->icon_name = "struct-field-symbolic";
      break;

    case CXCursor_VarDecl:
      /* local? */
    case CXCursor_ParmDecl:
    case CXCursor_ObjCIvarDecl:
    case CXCursor_ObjCPropertyDecl:
    case CXCursor_ObjCSynthesizeDecl:
    case CXCursor_NonTypeTemplateParameter:
    case CXCursor_Namespace:
    case CXCursor_NamespaceAlias:
    case CXCursor_NamespaceRef:
      break;

    case CXCursor_StructDecl:
      self->icon_name = "lang-struct-symbolic";
      break;

    case CXCursor_UnionDecl:
    case CXCursor_ClassDecl:
    case CXCursor_TypeRef:
    case CXCursor_TemplateRef:
    case CXCursor_TypedefDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
    case CXCursor_ObjCClassRef:
    case CXCursor_ObjCInterfaceDecl:
    case CXCursor_ObjCImplementationDecl:
    case CXCursor_ObjCCategoryDecl:
    case CXCursor_ObjCCategoryImplDecl:
    case CXCursor_ObjCProtocolDecl:
    case CXCursor_ObjCProtocolRef:
    case CXCursor_TemplateTypeParameter:
    case CXCursor_TemplateTemplateParameter:
      self->icon_name  = "lang-class-symbolic";
      break;

    case CXCursor_EnumConstantDecl:
      self->icon_name = "lang-enum-value-symbolic";
      break;

    case CXCursor_EnumDecl:
      self->icon_name = "lang-enum-symbolic";
      break;

    case CXCursor_NotImplemented:
    default:
      break;
    }

  /*
   * Walk the chunks, creating our snippet for insertion as well as our markup
   * for the row in the completion window.
   */
  for (i = 0; i < num_chunks; i++)
    {
      enum CXCompletionChunkKind kind;
      IdeSourceSnippetChunk *chunk;
      const gchar *text;
      g_autofree gchar *escaped = NULL;
      CXString cxstr;

      kind = clang_getCompletionChunkKind (result->CompletionString, i);
      cxstr = clang_getCompletionChunkText (result->CompletionString, i);
      text = clang_getCString (cxstr);

      if (text)
        escaped = g_markup_escape_text (text, -1);
      else
        escaped = g_strdup ("");

      switch (kind)
        {
        case CXCompletionChunk_Optional:
          break;

        case CXCompletionChunk_TypedText:
          g_string_append_printf (markup, "<b>%s</b>", escaped);
          chunk = ide_source_snippet_chunk_new ();
          ide_source_snippet_chunk_set_text (chunk, text);
          ide_source_snippet_chunk_set_text_set (chunk, TRUE);
          ide_source_snippet_add_chunk (snippet, chunk);
          g_clear_object (&chunk);
          break;

        case CXCompletionChunk_Text:
          g_string_append (markup, escaped);
          chunk = ide_source_snippet_chunk_new ();
          ide_source_snippet_chunk_set_text (chunk, text);
          ide_source_snippet_chunk_set_text_set (chunk, TRUE);
          ide_source_snippet_add_chunk (snippet, chunk);
          g_clear_object (&chunk);
          break;

        case CXCompletionChunk_Placeholder:
          g_string_append (markup, escaped);
          chunk = ide_source_snippet_chunk_new ();
          ide_source_snippet_chunk_set_text (chunk, text);
          ide_source_snippet_chunk_set_text_set (chunk, TRUE);
          ide_source_snippet_chunk_set_tab_stop (chunk, ++tab_stop);
          ide_source_snippet_add_chunk (snippet, chunk);
          g_clear_object (&chunk);
          break;

        case CXCompletionChunk_Informative:
          if (0 == g_strcmp0 (text, "const "))
            g_string_append (markup, text);
          break;

        case CXCompletionChunk_CurrentParameter:
          break;

        case CXCompletionChunk_LeftParen:
          g_string_append (markup, " ");
          chunk = ide_source_snippet_chunk_new ();
          ide_source_snippet_chunk_set_text (chunk, " ");
          ide_source_snippet_chunk_set_text_set (chunk, TRUE);
          ide_source_snippet_add_chunk (snippet, chunk);
          g_clear_object (&chunk);
          /* fall through */
        case CXCompletionChunk_RightParen:
        case CXCompletionChunk_LeftBracket:
        case CXCompletionChunk_RightBracket:
        case CXCompletionChunk_LeftBrace:
        case CXCompletionChunk_RightBrace:
        case CXCompletionChunk_LeftAngle:
        case CXCompletionChunk_RightAngle:
        case CXCompletionChunk_Comma:
        case CXCompletionChunk_Colon:
        case CXCompletionChunk_SemiColon:
        case CXCompletionChunk_Equal:
        case CXCompletionChunk_HorizontalSpace:
          g_string_append (markup, escaped);
          chunk = ide_source_snippet_chunk_new ();
          ide_source_snippet_chunk_set_text (chunk, text);
          ide_source_snippet_chunk_set_text_set (chunk, TRUE);
          ide_source_snippet_add_chunk (snippet, chunk);
          g_clear_object (&chunk);
          break;

        case CXCompletionChunk_VerticalSpace:
          g_string_append (markup, escaped);
          /* insert the vertical space */
          chunk = ide_source_snippet_chunk_new ();
          ide_source_snippet_chunk_set_text (chunk, text);
          ide_source_snippet_chunk_set_text_set (chunk, TRUE);
          ide_source_snippet_add_chunk (snippet, chunk);
          g_clear_object (&chunk);
          /* now perform indentation */
          chunk = ide_source_snippet_chunk_new ();
          ide_source_snippet_chunk_set_text (chunk, "\t");
          ide_source_snippet_chunk_set_text_set (chunk, TRUE);
          ide_source_snippet_add_chunk (snippet, chunk);
          g_clear_object (&chunk);
          break;

        case CXCompletionChunk_ResultType:
          g_string_append_printf (markup, "%s ", escaped);
          break;

        default:
          break;
        }
    }

  self->snippet = g_object_ref (snippet);
  self->markup = g_string_free (markup, FALSE);
}

static gchar *
ide_clang_completion_item_get_markup (GtkSourceCompletionProposal *proposal)
{
  IdeClangCompletionItem *self = (IdeClangCompletionItem *)proposal;

  g_assert (IDE_IS_CLANG_COMPLETION_ITEM (self));

  ide_clang_completion_item_lazy_init (self);

  return g_strdup (self->markup);
}

static const gchar *
ide_clang_completion_item_get_icon_name (GtkSourceCompletionProposal *proposal)
{
  IdeClangCompletionItem *self = (IdeClangCompletionItem *)proposal;

  g_assert (IDE_IS_CLANG_COMPLETION_ITEM (self));

  ide_clang_completion_item_lazy_init (self);

  return self->icon_name;
}

static void
ide_clang_completion_item_finalize (GObject *object)
{
  IdeClangCompletionItem *self = (IdeClangCompletionItem *)object;

  g_clear_object (&self->snippet);
  g_clear_pointer (&self->brief_comment, g_free);
  g_clear_pointer (&self->typed_text, g_free);
  g_clear_pointer (&self->markup, g_free);
  g_clear_pointer (&self->results, ide_ref_ptr_unref);

  G_OBJECT_CLASS (ide_clang_completion_item_parent_class)->finalize (object);
}

static void
ide_clang_completion_item_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  IdeClangCompletionItem *self = IDE_CLANG_COMPLETION_ITEM (object);

  switch (prop_id)
    {
    case PROP_INDEX:
      g_value_set_uint (value, self->index);
      break;

    case PROP_RESULTS:
      g_value_set_boxed (value, self->results);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ide_clang_completion_item_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  IdeClangCompletionItem *self = IDE_CLANG_COMPLETION_ITEM (object);

  switch (prop_id)
    {
    case PROP_INDEX:
      self->index = g_value_get_uint (value);
      break;

    case PROP_RESULTS:
      self->results = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
completion_proposal_iface_init (GtkSourceCompletionProposalIface *iface)
{
  iface->get_icon_name = ide_clang_completion_item_get_icon_name;
  iface->get_markup = ide_clang_completion_item_get_markup;
}

static void
ide_clang_completion_item_class_init (IdeClangCompletionItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ide_clang_completion_item_finalize;
  object_class->get_property = ide_clang_completion_item_get_property;
  object_class->set_property = ide_clang_completion_item_set_property;

  gParamSpecs [PROP_INDEX] =
    g_param_spec_uint ("index",
                         "Index",
                         "The index in the result set.",
                         0,
                         G_MAXUINT-1,
                         0,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  gParamSpecs [PROP_RESULTS] =
    g_param_spec_boxed ("results",
                         "Results",
                         "The Clang result set.",
                         IDE_TYPE_REF_PTR,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, LAST_PROP, gParamSpecs);
}

static void
ide_clang_completion_item_init (IdeClangCompletionItem *self)
{
  self->typed_text_index = -1;
}

/**
 * ide_clang_completion_item_get_snippet:
 * @self: A #IdeClangCompletionItem.
 *
 * Gets the #IdeSourceSnippet to be inserted when expanding this completion item.
 *
 * Returns: (transfer none): An #IdeSourceSnippet.
 */
IdeSourceSnippet *
ide_clang_completion_item_get_snippet (IdeClangCompletionItem *self)
{
  g_return_val_if_fail (IDE_IS_CLANG_COMPLETION_ITEM (self), NULL);

  return self->snippet;
}

/**
 * ide_clang_completion_item_get_priority:
 * @self: A #IdeClangCompletionItem.
 *
 * Gets the completion priority for sorting within the results.
 *
 * Returns: An unsigned integer.
 */
guint
ide_clang_completion_item_get_priority (IdeClangCompletionItem *self)
{
  CXCompletionResult *result;

  g_return_val_if_fail (IDE_IS_CLANG_COMPLETION_ITEM (self), 0);

  result = ide_clang_completion_item_get_result (self);

  return clang_getCompletionPriority (result);
}

/**
 * ide_clang_completion_item_get_typed_text:
 * @self: An #IdeClangCompletionItem.
 *
 * Gets the text that would be expected to be typed to insert this completion
 * item into the text editor.
 *
 * Returns: A string which should not be modified or freed.
 */
const gchar *
ide_clang_completion_item_get_typed_text (IdeClangCompletionItem *self)
{
  CXCompletionResult *result;
  CXString cxstr;

  g_return_val_if_fail (IDE_IS_CLANG_COMPLETION_ITEM (self), NULL);

  if (self->typed_text)
    return self->typed_text;

  result = ide_clang_completion_item_get_result (self);

  /*
   * Determine the index of the typed text. Each completion result should have
   * exaction one of these.
   */
  if (G_UNLIKELY (self->typed_text_index == -1))
    {
      guint num_chunks;
      guint i;

      num_chunks = clang_getNumCompletionChunks (result);

      for (i = 0; i < num_chunks; i++)
        {
          enum CXCompletionChunkKind kind;

          kind = clang_getCompletionChunkKind (result->CompletionString, i);
          if (kind == CXCompletionChunk_TypedText)
            {
              self->typed_text_index = i;
              break;
            }
        }
    }

  if (self->typed_text_index == -1)
    {
      /*
       * FIXME:
       *
       * This seems like an implausible result, but we are definitely
       * hitting it occasionally.
       */
      return g_strdup ("");
    }

#ifdef IDE_ENABLE_TRACE
  {
    enum CXCompletionChunkKind kind;
    unsigned num_chunks;

    g_assert (self->typed_text_index >= 0);

    num_chunks = clang_getNumCompletionChunks (result->CompletionString);
    g_assert (num_chunks > self->typed_text_index);

    kind = clang_getCompletionChunkKind (result->CompletionString, self->typed_text_index);
    g_assert (kind == CXCompletionChunk_TypedText);
  }
#endif

  cxstr = clang_getCompletionChunkText (result->CompletionString, self->typed_text_index);
  self->typed_text = g_strdup (clang_getCString (cxstr));
  clang_disposeString (cxstr);

  return self->typed_text;
}

/**
 * ide_clang_completion_item_get_brief_comment:
 * @self: An #IdeClangCompletionItem.
 *
 * Gets the brief comment that can be used to show extra information for the
 * result.
 *
 * Returns: A string which should not be modified or freed.
 */
const gchar *
ide_clang_completion_item_get_brief_comment (IdeClangCompletionItem *self)
{
  CXCompletionResult *result;

  g_return_val_if_fail (IDE_IS_CLANG_COMPLETION_ITEM (self), NULL);

  if (self->brief_comment == NULL)
    {
      CXString cxstr;

      result = ide_clang_completion_item_get_result (self);
      cxstr = clang_getCompletionBriefComment (result->CompletionString);
      self->brief_comment = g_strdup (clang_getCString (cxstr));
      clang_disposeString (cxstr);
    }

  return self->brief_comment;
}