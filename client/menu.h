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

#ifndef _H_MENU
#define _H_MENU

class CommandTarget {
public:
    virtual void do_command(int command) = 0;
    virtual ~CommandTarget() {}
};

class Menu {
public:
    Menu(CommandTarget& target, const std::string& name);

    enum ItemType {
        MENU_ITEM_TYPE_INVALID,
        MENU_ITEM_TYPE_COMMAND,
        MENU_ITEM_TYPE_MENU,
        MENU_ITEM_TYPE_SEPARATOR,
    };

    Menu* ref() { _refs++; return this;}
    void unref() { if (!--_refs) delete this;}

    const std::string& get_name() { return _name;}
    CommandTarget& get_target() { return _target;}

    void add_command(const std::string& name, int cmd_id);
    void add_separator();
    void add_sub(Menu* sub);

    ItemType item_type_at(int pos);
    void command_at(int pos, std::string& name, int& cmd_id);
    Menu* sub_at(int pos);

private:
    virtual ~Menu();

    class MenuCommand {
    public:
        MenuCommand(const std::string& name, int cmd_id)
            : _name (name)
            , _cmd_id (cmd_id)
        {
        }

        const std::string& get_name() { return _name;}
        int get_cmd_id() { return _cmd_id;}

    private:
        std::string _name;
        int _cmd_id;
    };

    struct MenuItem {
        ItemType type;
        void *obj;
    };

    void add_item(MenuItem& item);

private:
    int _refs;
    CommandTarget& _target;
    std::string _name;
    std::vector<MenuItem> _items;
};

#endif

