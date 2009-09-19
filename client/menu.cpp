/*
   Copyright (C) 2009 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common.h"
#include "menu.h"
#include "utils.h"
#include "debug.h"


Menu::Menu(CommandTarget& target, const std::string& name)
    : _refs (1)
    , _target (target)
    , _name (name)
{
}

Menu::~Menu()
{
    for (unsigned int i = 0; i < _items.size(); i++) {
        if (_items[i].type == MENU_ITEM_TYPE_COMMAND) {
            delete (MenuCommand*)_items[i].obj;
        } else if (_items[i].type == MENU_ITEM_TYPE_MENU) {
            ((Menu*)_items[i].obj)->unref();
        }
    }
}

void Menu::add_item(MenuItem& item)
{
    int pos = _items.size();
    _items.resize(pos + 1);
    _items[pos] = item;
}

void Menu::add_command(const std::string& name, int cmd_id)
{
    MenuCommand* cmd = new MenuCommand(name, cmd_id);
    MenuItem item;
    item.type = MENU_ITEM_TYPE_COMMAND;
    item.obj = cmd;
    add_item(item);
}

void Menu::add_separator()
{
    MenuItem item;
    item.type = MENU_ITEM_TYPE_SEPARATOR;
    item.obj = NULL;
    add_item(item);
}

void Menu::add_sub(Menu* menu)
{
    ASSERT(menu);
    MenuItem item;
    item.type = MENU_ITEM_TYPE_MENU;
    item.obj = menu->ref();
    add_item(item);
}

Menu::ItemType Menu::item_type_at(int pos)
{
    if (pos >= (int)_items.size()) {
        return MENU_ITEM_TYPE_INVALID;
    }
    return _items[pos].type;
}

void Menu::command_at(int pos, std::string& name, int& cmd_id)
{
    if (_items[pos].type != MENU_ITEM_TYPE_COMMAND) {
        THROW("incorrect item type");
    }
    MenuCommand* cmd = (MenuCommand*)_items[pos].obj;
    name = cmd->get_name();
    cmd_id = cmd->get_cmd_id();
}

Menu* Menu::sub_at(int pos)
{
    if (_items[pos].type != MENU_ITEM_TYPE_MENU) {
        THROW("incorrect item type");
    }
    return ((Menu*)_items[pos].obj)->ref();
}

