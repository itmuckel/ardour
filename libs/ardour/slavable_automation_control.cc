/*
    Copyright (C) 2016 Paul Davis

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2 of the License, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifndef __libardour_slavable_automation_control_h__
#define __libardour_slavable_automation_control_h__

#include "pbd/enumwriter.h"
#include "pbd/error.h"
#include "pbd/types_convert.h"
#include "pbd/i18n.h"

#include "evoral/Curve.hpp"

#include "ardour/audioengine.h"
#include "ardour/runtime_functions.h"
#include "ardour/slavable_automation_control.h"
#include "ardour/session.h"

using namespace std;
using namespace ARDOUR;
using namespace PBD;

SlavableAutomationControl::SlavableAutomationControl(ARDOUR::Session& s,
                                                     const Evoral::Parameter&                  parameter,
                                                     const ParameterDescriptor&                desc,
                                                     boost::shared_ptr<ARDOUR::AutomationList> l,
                                                     const std::string&                        name,
                                                     Controllable::Flag                        flags)
	: AutomationControl (s, parameter, desc, l, name, flags)
	, _masters_node (0)
{
}

SlavableAutomationControl::~SlavableAutomationControl ()
{
	if (_masters_node) {
		delete _masters_node;
		_masters_node = 0;
	}
}

double
SlavableAutomationControl::get_masters_value_locked () const
{
	if (_desc.toggled) {
		for (Masters::const_iterator mr = _masters.begin(); mr != _masters.end(); ++mr) {
			if (mr->second.master()->get_value()) {
				return _desc.upper;
			}
		}
		return _desc.lower;
	} else {

		double v = 1.0; /* the masters function as a scaling factor */

		for (Masters::const_iterator mr = _masters.begin(); mr != _masters.end(); ++mr) {
			v *= mr->second.master_ratio ();
		}

		return v;
	}
}

double
SlavableAutomationControl::get_value_locked() const
{
	/* read or write masters lock must be held */

	if (_masters.empty()) {
		return Control::get_double (false, _session.transport_frame());
	}

	if (_desc.toggled) {
		/* for boolean/toggle controls, if this slave OR any master is
		 * enabled, this slave is enabled. So check our own value
		 * first, because if we are enabled, we can return immediately.
		 */
		if (Control::get_double (false, _session.transport_frame())) {
			return _desc.upper;
		}
	}

	return Control::get_double() * get_masters_value_locked ();
}

/** Get the current effective `user' value based on automation state */
double
SlavableAutomationControl::get_value() const
{
	bool from_list = _list && boost::dynamic_pointer_cast<AutomationList>(_list)->automation_playback();

	Glib::Threads::RWLock::ReaderLock lm (master_lock);
	if (!from_list) {
		return get_value_locked ();
	} else {
		return Control::get_double (true, _session.transport_frame()) * get_masters_value_locked();
	}
}

bool
SlavableAutomationControl::get_masters_curve_locked (framepos_t, framepos_t, float*, framecnt_t) const
{
	/* Every AutomationControl needs to implement this as-needed.
	 *
	 * This class also provides some convenient methods which
	 * could be used as defaults here (depending on  AutomationType)
	 * e.g. masters_curve_multiply()
	 */
	return false;
}

bool
SlavableAutomationControl::masters_curve_multiply (framepos_t start, framepos_t end, float* vec, framecnt_t veclen) const
{
	gain_t* scratch = _session.scratch_automation_buffer ();
	bool rv = list()->curve().rt_safe_get_vector (start, end, scratch, veclen);
	if (rv) {
		for (framecnt_t i = 0; i < veclen; ++i) {
			vec[i] *= scratch[i];
		}
	} else {
		apply_gain_to_buffer (vec, veclen, Control::get_double ());
	}
	if (_masters.empty()) {
		return rv;
	}

	for (Masters::const_iterator mr = _masters.begin(); mr != _masters.end(); ++mr) {
		boost::shared_ptr<SlavableAutomationControl> sc
			= boost::dynamic_pointer_cast<SlavableAutomationControl>(mr->second.master());
		assert (sc);
		rv |= sc->masters_curve_multiply (start, end, vec, veclen);
		apply_gain_to_buffer (vec, veclen, mr->second.master_ratio ());
	}
	return rv;
}

void
SlavableAutomationControl::actually_set_value (double value, PBD::Controllable::GroupControlDisposition gcd)
{
	if (!_desc.toggled) {

		Glib::Threads::RWLock::WriterLock lm (master_lock);

		if (!_masters.empty()) {
			/* need to scale given value by current master's scaling */
			const double masters_value = get_masters_value_locked();
			if (masters_value == 0.0) {
				value = 0.0;
			} else {
				value /= masters_value;
				value = std::max (lower(), std::min(upper(), value));
			}
		}
	}

	/* this will call Control::set_double() and emit Changed signals as appropriate */
	AutomationControl::actually_set_value (value, gcd);
}

void
SlavableAutomationControl::add_master (boost::shared_ptr<AutomationControl> m, bool loading)
{
	std::pair<Masters::iterator,bool> res;

	{
		const double master_value = m->get_value();
		Glib::Threads::RWLock::WriterLock lm (master_lock);

		pair<PBD::ID,MasterRecord> newpair (m->id(), MasterRecord (boost::weak_ptr<AutomationControl> (m), get_value_locked(), master_value));
		res = _masters.insert (newpair);

		if (res.second) {

			/* note that we bind @param m as a weak_ptr<AutomationControl>, thus
			   avoiding holding a reference to the control in the binding
			   itself.
			*/
			m->DropReferences.connect_same_thread (res.first->second.dropped_connection, boost::bind (&SlavableAutomationControl::master_going_away, this, boost::weak_ptr<AutomationControl>(m)));

			/* Store the connection inside the MasterRecord, so
			   that when we destroy it, the connection is destroyed
			   and we no longer hear about changes to the
			   AutomationControl.

			   Note that this also makes it safe to store a
			   boost::shared_ptr<AutomationControl> in the functor,
			   since we know we will destroy the functor when the
			   connection is destroyed, which happens when we
			   disconnect from the master (for any reason).

			   Note that we fix the "from_self" argument that will
			   be given to our own Changed signal to "false",
			   because the change came from the master.
			*/

			m->Changed.connect_same_thread (res.first->second.changed_connection, boost::bind (&SlavableAutomationControl::master_changed, this, _1, _2, boost::weak_ptr<AutomationControl>(m)));
		}
	}

	if (res.second) {
		/* this will notify everyone that we're now slaved to the master */
		MasterStatusChange (); /* EMIT SIGNAL */
	}

	post_add_master (m);

	update_boolean_masters_records (m);
}

int32_t
SlavableAutomationControl::get_boolean_masters () const
{
	int32_t n = 0;

	if (_desc.toggled) {
		Glib::Threads::RWLock::ReaderLock lm (master_lock);
		for (Masters::const_iterator mr = _masters.begin(); mr != _masters.end(); ++mr) {
			if (mr->second.yn()) {
				++n;
			}
		}
	}

	return n;
}

void
SlavableAutomationControl::update_boolean_masters_records (boost::shared_ptr<AutomationControl> m)
{
	if (_desc.toggled) {
		/* We may modify a MasterRecord, but we not modify the master
		 * map, so we use a ReaderLock
		 */
		Glib::Threads::RWLock::ReaderLock lm (master_lock);
		Masters::iterator mi = _masters.find (m->id());
		if (mi != _masters.end()) {
			/* update MasterRecord to show whether the master is
			   on/off. We need to store this because the master
			   may change (in the sense of emitting Changed())
			   several times without actually changing the result
			   of ::get_value(). This is a feature of
			   AutomationControls (or even just Controllables,
			   really) which have more than a simple scalar
			   value. For example, the master may be a mute control
			   which can be muted_by_self() and/or
			   muted_by_masters(). When either of those two
			   conditions changes, Changed() will be emitted, even
			   though ::get_value() will return the same value each
			   time (1.0 if either are true, 0.0 if neither is).

			   This provides a way for derived types to check
			   the last known state of a Master when the Master
			   changes. We update it after calling
			   ::master_changed() (though derived types must do
			   this themselves).
			*/
			mi->second.set_yn (m->get_value());
		}
	}
}

void
SlavableAutomationControl::master_changed (bool /*from_self*/, GroupControlDisposition gcd, boost::weak_ptr<AutomationControl> wm)
{
	boost::shared_ptr<AutomationControl> m = wm.lock ();
	assert (m);
	Glib::Threads::RWLock::ReaderLock lm (master_lock, Glib::Threads::TRY_LOCK);
	if (!lm.locked ()) {
		/* boolean_automation_run_locked () special case */
		return;
	}
	bool send_signal = handle_master_change (m);
	lm.release (); // update_boolean_masters_records() takes lock

	update_boolean_masters_records (m);
	if (send_signal) {
		Changed (false, Controllable::NoGroup); /* EMIT SIGNAL */
	}
}

void
SlavableAutomationControl::master_going_away (boost::weak_ptr<AutomationControl> wm)
{
	boost::shared_ptr<AutomationControl> m = wm.lock();
	if (m) {
		remove_master (m);
	}
}

void
SlavableAutomationControl::remove_master (boost::shared_ptr<AutomationControl> m)
{
	if (_session.deletion_in_progress()) {
		/* no reason to care about new values or sending signals */
		return;
	}

	pre_remove_master (m);
	double new_val = AutomationControl::get_double();
	const double old_val = new_val;

	{
		Glib::Threads::RWLock::WriterLock lm (master_lock);

		Masters::const_iterator mi = _masters.find (m->id ());

		/* when un-assigning we apply the master-value permanently */
		if (mi != _masters.end()) {
			new_val *= mi->second.master_ratio ();
		}

		if (!_masters.erase (m->id())) {
			return;
		}
	}

	if (old_val != new_val) {
		AutomationControl::set_double (new_val, Controllable::NoGroup);
	}

	MasterStatusChange (); /* EMIT SIGNAL */

	/* no need to update boolean masters records, since the MR will have
	 * been removed already.
	 */
}

void
SlavableAutomationControl::clear_masters ()
{
	if (_session.deletion_in_progress()) {
		/* no reason to care about new values or sending signals */
		return;
	}

	double new_val = AutomationControl::get_double();
	const double old_val = new_val;

	/* null ptr means "all masters */
	pre_remove_master (boost::shared_ptr<AutomationControl>());

	{
		Glib::Threads::RWLock::WriterLock lm (master_lock);
		if (_masters.empty()) {
			return;
		}
		/* permanently apply masters value */
		new_val *= get_masters_value_locked ();

		_masters.clear ();
	}

	if (old_val != new_val) {
		AutomationControl::set_double (new_val, Controllable::NoGroup);
	}
	MasterStatusChange (); /* EMIT SIGNAL */

	/* no need to update boolean masters records, since all MRs will have
	 * been removed already.
	 */
}

bool
SlavableAutomationControl::find_next_event_locked (double now, double end, Evoral::ControlEvent& next_event) const
{
	if (_masters.empty()) {
		return false;
	}
	bool rv = false;
	/* iterate over all masters check their automation lists
	 * for any event between "now" and "end" which is earlier than
	 * next_event.when. If found, set next_event.when and return true.
	 * (see also Automatable::find_next_event)
	 */
	for (Masters::const_iterator mr = _masters.begin(); mr != _masters.end(); ++mr) {
		boost::shared_ptr<AutomationControl> ac (mr->second.master());

		boost::shared_ptr<SlavableAutomationControl> sc
			= boost::dynamic_pointer_cast<SlavableAutomationControl>(ac);

		if (sc && sc->find_next_event_locked (now, end, next_event)) {
			rv = true;
		}

		Evoral::ControlList::const_iterator i;
		boost::shared_ptr<const Evoral::ControlList> alist (ac->list());
		Evoral::ControlEvent cp (now, 0.0f);
		if (!alist) {
			continue;
		}

		for (i = lower_bound (alist->begin(), alist->end(), &cp, Evoral::ControlList::time_comparator);
		     i != alist->end() && (*i)->when < end; ++i) {
			if ((*i)->when > now) {
				break;
			}
		}

		if (i != alist->end() && (*i)->when < end) {
			if ((*i)->when < next_event.when) {
				next_event.when = (*i)->when;
				rv = true;
			}
		}
	}

	return rv;
}

bool
SlavableAutomationControl::handle_master_change (boost::shared_ptr<AutomationControl>)
{
	/* Derived classes can implement this for special cases (e.g. mute).
	 * This method is called with a ReaderLock (master_lock) held.
	 *
	 * return true if the changed master value resulted
	 * in a change of the control itself. */
	return true; // emit Changed
}

bool
SlavableAutomationControl::boolean_automation_run_locked (framepos_t start, pframes_t len)
{
	bool rv = false;
	if (!_desc.toggled) {
		return false;
	}
	for (Masters::iterator mr = _masters.begin(); mr != _masters.end(); ++mr) {
		boost::shared_ptr<AutomationControl> ac (mr->second.master());
		if (!ac->automation_playback ()) {
			continue;
		}
		if (!ac->toggled ()) {
			continue;
		}
		boost::shared_ptr<SlavableAutomationControl> sc = boost::dynamic_pointer_cast<MuteControl>(ac);
		if (sc) {
			rv |= sc->boolean_automation_run (start, len);
		}
		boost::shared_ptr<const Evoral::ControlList> alist (ac->list());
		bool valid = false;
		const bool yn = alist->rt_safe_eval (start, valid) >= 0.5;
		if (!valid) {
			continue;
		}
		/* ideally we'd call just master_changed() which calls update_boolean_masters_records()
		 * but that takes the master_lock, which is already locked */
		if (mr->second.yn() != yn) {
			rv |= handle_master_change (ac);
			mr->second.set_yn (yn);
			/* notify the GUI, without recursion:
			 * master_changed() above will ignore the change if the lock is held.
			 */
			ac->set_value_unchecked (yn ? 1. : 0.);
			ac->Changed (false, Controllable::NoGroup); /* EMIT SIGNAL */
		}
	}
	return rv;
}

bool
SlavableAutomationControl::boolean_automation_run (framepos_t start, pframes_t len)
{
	bool change = false;
	{
		 Glib::Threads::RWLock::ReaderLock lm (master_lock);
		 change = boolean_automation_run_locked (start, len);
	}
	if (change) {
		Changed (false, Controllable::NoGroup); /* EMIT SIGNAL */
	}
	return change;
}

bool
SlavableAutomationControl::slaved_to (boost::shared_ptr<AutomationControl> m) const
{
	Glib::Threads::RWLock::ReaderLock lm (master_lock);
	return _masters.find (m->id()) != _masters.end();
}

bool
SlavableAutomationControl::slaved () const
{
	Glib::Threads::RWLock::ReaderLock lm (master_lock);
	return !_masters.empty();
}

int
SlavableAutomationControl::MasterRecord::set_state (XMLNode const& n, int)
{
	n.get_property (X_("yn"), _yn);
	n.get_property (X_("val-ctrl"), _val_ctrl);
	n.get_property (X_("val-master"), _val_master);
	return 0;
}

void
SlavableAutomationControl::use_saved_master_ratios ()
{
	if (!_masters_node) {
		return;
	}

	Glib::Threads::RWLock::ReaderLock lm (master_lock);

	XMLNodeList nlist = _masters_node->children();
	XMLNodeIterator niter;

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
		ID id_val;
		if (!(*niter)->get_property (X_("id"), id_val)) {
			continue;
		}
		Masters::iterator mi = _masters.find (id_val);
		if (mi == _masters.end()) {
			continue;
		}
		mi->second.set_state (**niter, Stateful::loading_state_version);
	}

	delete _masters_node;
	_masters_node = 0;

	return;
}


XMLNode&
SlavableAutomationControl::get_state ()
{
	XMLNode& node (AutomationControl::get_state());

	/* store VCA master ratios */

	{
		Glib::Threads::RWLock::ReaderLock lm (master_lock);
		if (!_masters.empty()) {
			XMLNode* masters_node = new XMLNode (X_("masters"));
			for (Masters::iterator mr = _masters.begin(); mr != _masters.end(); ++mr) {
				XMLNode* mnode = new XMLNode (X_("master"));
				mnode->set_property (X_("id"), mr->second.master()->id());

				if (_desc.toggled) {
					mnode->set_property (X_("yn"), mr->second.yn());
				} else {
					mnode->set_property (X_("val-ctrl"), mr->second.val_ctrl());
					mnode->set_property (X_("val-master"), mr->second.val_master());
				}
				masters_node->add_child_nocopy (*mnode);
			}
			node.add_child_nocopy (*masters_node);
		}
	}

	return node;
}

int
SlavableAutomationControl::set_state (XMLNode const& node, int version)
{
	XMLNodeList nlist = node.children();
	XMLNodeIterator niter;

	for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
		if ((*niter)->name() == X_("masters")) {
			_masters_node = new XMLNode (**niter);
		}
	}

	return AutomationControl::set_state (node, version);
}


#endif /* __libardour_slavable_automation_control_h__ */
