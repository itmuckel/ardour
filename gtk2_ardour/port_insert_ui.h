/*
 * Copyright (C) 2014-2017 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef __gtkardour_port_insert_ui_h__
#define __gtkardour_port_insert_ui_h__

#include "widgets/stateful_button.h"
#include "ardour_dialog.h"
#include "io_selector.h"

namespace ARDOUR {
	class PortInsert;
}

class PortInsertUI : public Gtk::VBox
{
public:
	PortInsertUI (Gtk::Window*, ARDOUR::Session *, std::shared_ptr<ARDOUR::PortInsert>);

	void redisplay ();
	void finished (IOSelector::Result);

private:
	std::shared_ptr<ARDOUR::PortInsert> _pi;

	Gtk::Notebook notebook;
	ArdourWidgets::StatefulToggleButton latency_button;
	IOSelector input_selector;
	IOSelector output_selector;
	Gtk::Label latency_display;
	Gtk::HBox  latency_hbox;
	sigc::connection latency_timeout;

	bool check_latency_measurement ();
	void latency_button_toggled ();
	void update_latency_display ();
};

class PortInsertWindow : public ArdourDialog
{
  public:
	PortInsertWindow (ARDOUR::Session *, std::shared_ptr<ARDOUR::PortInsert>);

  protected:
	void on_map ();

  private:
	PortInsertUI _portinsertui;
	Gtk::VBox vbox;

	void cancel ();
	void accept ();

	PBD::ScopedConnection going_away_connection;

	bool wm_delete (GdkEventAny*);
};

#endif /* __gtkardour_port_insert_ui_h__ */
