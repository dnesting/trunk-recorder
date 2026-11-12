/* -*- c++ -*- */

/*
 * Copyright 2020 Free Software Foundation, Inc.
 *
 * This file is part of GNU Radio
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "plugin_wrapper_impl.h"
#include "plugin_wrapper.h"
#include <algorithm>
#include <boost/math/special_functions/round.hpp>
#include <climits>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <gnuradio/io_signature.h>
#include <gnuradio/thread/thread.h>
#include <stdexcept>
#include <stdio.h>

namespace gr {
namespace blocks {

plugin_wrapper_impl::sptr
plugin_wrapper_impl::make(plugin_callback callback) {
  return gnuradio::get_initial_sptr(new plugin_wrapper_impl(callback));
}

plugin_wrapper_impl::plugin_wrapper_impl(plugin_callback callback)
    : sync_block("plugin_wrapper_impl",
                 io_signature::make(1, 1, sizeof(int16_t)),
                 io_signature::make(0, 0, 0)),
      d_callback(callback) {}

void plugin_wrapper_impl::set_talkgroup_gate(long talkgroup) {
  gr::thread::scoped_lock guard(d_mutex);
  d_gate.enable(talkgroup);
}

void plugin_wrapper_impl::disable_talkgroup_gate() {
  gr::thread::scoped_lock guard(d_mutex);
  d_gate.disable();
}

int plugin_wrapper_impl::work(int noutput_items, gr_vector_const_void_star &input_items, gr_vector_void_star &output_items) {

  gr::thread::scoped_lock guard(d_mutex); // hold mutex for duration of this

  return dowork(noutput_items, input_items, output_items);
}

int plugin_wrapper_impl::dowork(int noutput_items, gr_vector_const_void_star &input_items, gr_vector_void_star &output_items) {

  if (d_callback != NULL) {
    std::vector<Talkgroup_Gate::Tag> gate_tags;
    if (d_gate.enabled()) {
      static const pmt::pmt_t grp_id_key(pmt::intern("grp_id"));
      static const pmt::pmt_t terminate_key(pmt::intern("terminate"));
      std::vector<gr::tag_t> tags;
      get_tags_in_window(tags, 0, 0, noutput_items);
      std::stable_sort(tags.begin(), tags.end(), gr::tag_t::offset_compare);
      for (const gr::tag_t &tag : tags) {
        int offset = tag.offset - nitems_read(0);
        if (pmt::eq(grp_id_key, tag.key)) {
          gate_tags.push_back({offset, Talkgroup_Gate::GRP_ID, pmt::to_long(tag.value)});
        } else if (pmt::eq(terminate_key, tag.key)) {
          gate_tags.push_back({offset, Talkgroup_Gate::TERMINATE, 0});
        }
      }
    }
    Talkgroup_Gate::Result result = d_gate.process(noutput_items, gate_tags);
    if (result.dropped > 0) {
      BOOST_LOG_TRIVIAL(debug) << "plugin_wrapper_impl GROUP MISMATCH - Expected TG: " << d_gate.talkgroup() << " Received TG: " << result.received_talkgroup << " dropped: " << result.dropped << " incoming: " << noutput_items;
    }
    const int16_t *samples = (const int16_t *)input_items[0];
    for (const Talkgroup_Gate::Run &run : result.runs) {
      d_callback((int16_t *)samples + run.start, run.end - run.start);
    }
  } else {
    BOOST_LOG_TRIVIAL(warning) << "plugin_wrapper_impl dropped, no callback setup!";
  }

  return noutput_items;
}

} /* namespace blocks */
} /* namespace gr */