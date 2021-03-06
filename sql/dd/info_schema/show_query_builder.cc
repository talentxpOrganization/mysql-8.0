/* Copyright (c) 2016, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "dd/info_schema/show_query_builder.h" // Select_lex_builder

#include "item_cmpfunc.h"                      // Item_func_like
#include "item_func.h"
#include "m_string.h"                          // C_STRING_WITH_LEN
#include "my_dbug.h"
#include "parse_tree_helpers.h"
#include "parse_tree_items.h"                  // PTI_simple_ident_ident
#include "parse_tree_nodes.h"                  // PT_select_item_list
#include "sql_lex.h"                           // Query_options
#include "sql_string.h"

class Item;


namespace dd {
namespace info_schema {

static const Query_options options=
{
  0, /* query_spec_options */
  SELECT_LEX::SQL_CACHE_UNSPECIFIED /* sql_cache */
};


Select_lex_builder::Select_lex_builder(const POS *pc, THD *thd)
  :m_pos(pc),
   m_thd(thd),
   m_select_item_list(nullptr),
   m_where_clause(nullptr),
   m_order_by_list(nullptr)
{
  m_table_reference_list.init(m_thd->mem_root);
}


bool Select_lex_builder::add_to_select_item_list(Item *expr)
{
  // Prepare list if not exist.
  if (!m_select_item_list)
  {
    m_select_item_list= new (m_thd->mem_root) PT_select_item_list();

    if (m_select_item_list == nullptr)
      return true;
  }

  m_select_item_list->push_back(expr);

  return false;
}


// Add item representing star in "SELECT '*' ...".
bool Select_lex_builder::add_star_select_item()
{
  const LEX_STRING star= { C_STRING_WITH_LEN("*") };

  PTI_simple_ident_ident *ident_star;
  ident_star= new (m_thd->mem_root) PTI_simple_ident_ident(*m_pos, star);
  if (ident_star == nullptr)
    return true;

  return add_to_select_item_list(ident_star);
}


/**
  Add item representing a column as,
  "SELECT <field_name> AS <alias>, ...".
*/
bool Select_lex_builder::add_select_item(const LEX_STRING field_name,
                                         const LEX_STRING alias)
{
  /* ... FIELD_NAME ... */
  PTI_simple_ident_ident *ident_field;
  ident_field= new (m_thd->mem_root)
    PTI_simple_ident_ident(*m_pos, field_name);
  if (ident_field== nullptr)
    return true;

  /* ... FIELD_NAME as alias ... */
  PTI_expr_with_alias *expr;
  expr= new (m_thd->mem_root) PTI_expr_with_alias(*m_pos,
                                                  ident_field,
                                                  m_pos->cpp,
                                                  alias);
  if (expr== nullptr)
    return true;

  return add_to_select_item_list(expr);
}


/**
  Add item representing a FROM clause table as,
  "SELECT ... FROM <schema_name>.<table_name> ...".
*/
bool Select_lex_builder::add_from_item(const LEX_STRING schema_name,
                                       const LEX_STRING table_name)
{
  /*
    make_table_list() might alter the database and table name
    strings. Create copies and leave the original values
    unaltered.
  */

  /* ... schame_name ... */
  LEX_CSTRING tmp_db_name;
  if (!m_thd->make_lex_string(&tmp_db_name,
                            schema_name.str,
                            schema_name.length,
                            false))
    return true;

  /* ... <table_name> ... */
  LEX_CSTRING tmp_table_name;
  if (!m_thd->make_lex_string(&tmp_table_name,
                            table_name.str,
                            table_name.length, false))
    return true;

  /* ... schame_name.<table_name> ... */
  Table_ident *table_ident;
  table_ident= new (m_thd->mem_root) Table_ident(tmp_db_name,
                                                 tmp_table_name);
  if (table_ident == nullptr)
    return true;

  /* ... FROM schame_name.<table_name> ... */
  PT_table_factor_table_ident *table_factor;
  table_factor= new (m_thd->mem_root)
    PT_table_factor_table_ident(table_ident,
                                nullptr, nullptr, nullptr);
  if (table_factor == nullptr)
    return true;

  if (m_table_reference_list.push_back(table_factor))
    return true;

  return false;
}


/**
  Add item representing a FROM clause table as,
  "SELECT ... FROM <sub query or derived table> ...".
 */
bool Select_lex_builder::add_from_item(PT_derived_table *dt)
{
  if (m_table_reference_list.push_back(dt))
    return true;

  return false;
}


// Prepare item representing a LIKE condition.
Item *Select_lex_builder::prepare_like_item(const LEX_STRING field_name,
                                            const String *wild)
{
  /* ... FIELD_NAME ... */
  PTI_simple_ident_ident *ident_field;
  ident_field= new (m_thd->mem_root)
    PTI_simple_ident_ident(*m_pos, field_name);
  if (ident_field == nullptr)
    return nullptr;

  /* ... <value> ... */
  LEX_STRING *lex_string;
  lex_string= static_cast<LEX_STRING*> (m_thd->alloc(sizeof(LEX_STRING)));
  if (lex_string == nullptr)
    return nullptr;
  lex_string->length= wild->length();
  lex_string->str= m_thd->strmake(wild->ptr(), wild->length());
  if (lex_string->str == nullptr)
    return nullptr;

  PTI_text_literal_text_string *wild_string;
  wild_string= new (m_thd->mem_root)
    PTI_text_literal_text_string(*m_pos, false, *lex_string); // TODO WL#6629 check is_7bit
  if (wild_string == nullptr)
    return nullptr;

  /* ... field_name LIKE <value> ... */
  Item_func_like *func_like;
  func_like= new (m_thd->mem_root) Item_func_like(*m_pos,
                                                  ident_field,
                                                  wild_string,
                                                  nullptr);

  return func_like;
}


// Prepare item representing a equal to comparision condition.
Item *Select_lex_builder::prepare_equal_item(const LEX_STRING field_name,
                                             const LEX_STRING value)
{
  /* ... FIELD_NAME ... */
  PTI_simple_ident_ident *ident_field;
  ident_field= new (m_thd->mem_root)
                 PTI_simple_ident_ident(*m_pos, field_name);
  if (ident_field == nullptr)
    return nullptr;

  /* ... <value> ... */
  LEX_STRING *lex_string;
  lex_string= static_cast<LEX_STRING*> (m_thd->alloc(sizeof(LEX_STRING)));
  if (lex_string == nullptr)
    return nullptr;
  lex_string->length= value.length;
  lex_string->str= m_thd->strmake(value.str, value.length);
  if (lex_string->str == nullptr)
    return nullptr;

  PTI_text_literal_underscore_charset *value_string;
  value_string= new (m_thd->mem_root)
    PTI_text_literal_underscore_charset(
      *m_pos, false, system_charset_info, *lex_string); // TODO WL#6629 check is_7bit
  if (value_string == nullptr)
    return nullptr;

  /* ... FIELD_NAME = <value> ... */
  Item_func_eq *func_eq=
    new (m_thd->mem_root) Item_func_eq(*m_pos, ident_field, value_string);

  return func_eq;
}


// Add a WHERE clause condition to Select_lex_builder.
bool Select_lex_builder::add_condition(Item *a)
{
  DBUG_ASSERT(a != nullptr);

  /* ... WHERE cond ... */
  if (m_where_clause == nullptr)
  {
    m_where_clause= a;
  }
  /* ... WHERE <cond> AND <cond> ... */
  else
  {
    m_where_clause=
      flatten_associative_operator<Item_cond_and, Item_func::COND_AND_FUNC>(
        m_thd->mem_root, *m_pos, m_where_clause, a);

    if (m_where_clause == nullptr)
      return true;
  }

  return false;
}


// Add a ORDER BY clause field to Select_lex_builder.
bool Select_lex_builder::add_order_by(const LEX_STRING field_name)
{
  /* ... ORDER BY <field_name> ASC... */
  if (!m_order_by_list)
  {
    m_order_by_list= new (m_thd->mem_root) PT_order_list();
    if (m_order_by_list == nullptr)
      return true;
  }

  /* ... FIELD_NAME ... */
  PTI_simple_ident_ident *ident_field;
  ident_field= new (m_thd->mem_root)
    PTI_simple_ident_ident(*m_pos, field_name);
  if (ident_field== nullptr)
    return true;

  PT_order_expr *expression= new (m_thd->mem_root)
    PT_order_expr(ident_field, ORDER_ASC);
  m_order_by_list->push_back(expression);

  return expression == nullptr;
}


/**
  This function build ParseTree node that represents this
  Select_lex_builder as sub-query.
*/
PT_derived_table* Select_lex_builder::prepare_derived_table(
                                        const LEX_STRING table_alias)
{
  Item *where_cond= nullptr;
  if (m_where_clause != nullptr)
    where_cond= new (m_thd->mem_root)
                  PTI_context<CTX_WHERE>(*m_pos, m_where_clause);

  PT_query_primary *query_specification;
  query_specification= new (m_thd->mem_root)
    PT_query_specification(options,
                           m_select_item_list,
                           m_table_reference_list,
                           where_cond);

  if (query_specification == nullptr)
    return nullptr;

  PT_query_expression_body_primary *query_expression_body_primary;
  query_expression_body_primary= new (m_thd->mem_root)
    PT_query_expression_body_primary(query_specification);
  if (query_expression_body_primary == nullptr)
    return nullptr;

  PT_query_expression *query_expression;
  query_expression= new (m_thd->mem_root)
    PT_query_expression(query_expression_body_primary);
  if (query_expression == nullptr)
    return nullptr;

  PT_subquery *sub_query;
  sub_query= new (m_thd->mem_root) PT_subquery(*m_pos,
                                               query_expression);
  if (sub_query == nullptr)
    return nullptr;

  LEX_STRING *derived_table_name= nullptr;
  derived_table_name= m_thd->make_lex_string(derived_table_name,
                                             table_alias.str,
                                             table_alias.length, true);
  if (derived_table_name == nullptr)
    return nullptr;

  Create_col_name_list column_names;
  column_names.init(m_thd->mem_root);
  PT_derived_table *derived_table;
  derived_table= new (m_thd->mem_root)
    PT_derived_table(sub_query, derived_table_name, &column_names);

  return derived_table;
}


/**
  Prepare a SELECT_LEX using all the information information
  added to this Select_lex_builder.
*/
SELECT_LEX* Select_lex_builder::prepare_select_lex()
{
  Item *where_cond= nullptr;
  if (m_where_clause != nullptr)
    where_cond= new (m_thd->mem_root)
                  PTI_context<CTX_WHERE>(*m_pos, m_where_clause);

  PT_query_specification *query_specification2;
  query_specification2= new (m_thd->mem_root)
                          PT_query_specification(options,
                                                 m_select_item_list,
                                                 m_table_reference_list,
                                                 where_cond);
  if (query_specification2 == nullptr)
    return nullptr;

  PT_query_expression_body_primary *query_expression_body_primary2;
  query_expression_body_primary2= new (m_thd->mem_root)
    PT_query_expression_body_primary(query_specification2);
  if (query_expression_body_primary2 == nullptr)
    return nullptr;

  PT_order *pt_order_by= nullptr;
  if (m_order_by_list)
  {
    pt_order_by= new (m_thd->mem_root) PT_order(m_order_by_list);
    if (pt_order_by == nullptr)
      return nullptr;
  }

  PT_query_expression *query_expression2;
  query_expression2= new (m_thd->mem_root)
    PT_query_expression(query_expression_body_primary2,
                        pt_order_by, nullptr, nullptr);
  if (query_expression2 == nullptr)
    return nullptr;

  PT_select_stmt *select2;
  select2= new (m_thd->mem_root) PT_select_stmt(query_expression2);
  if (select2 == nullptr)
    return nullptr;

  LEX *lex= m_thd->lex;
  SELECT_LEX *current_select= lex->current_select();
  Parse_context pc(m_thd, current_select);
  if (m_thd->is_error())
    return nullptr;

  if (select2->contextualize(&pc))
    return nullptr;

  return current_select;
}

} // namespace info_schema
} // namespace dd
