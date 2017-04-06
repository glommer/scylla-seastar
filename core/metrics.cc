/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2016 ScyllaDB.
 */

#include "metrics.hh"
#include "metrics_api.hh"
#include <boost/range/algorithm.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/range/algorithm_ext/erase.hpp>

namespace seastar {
namespace metrics {

metric_groups::metric_groups() : _impl(impl::create_metric_groups()) {
}


metric_groups& metric_groups::add_group(const group_name_type& name, const std::initializer_list<metric_definition>& l) {
    _impl->add_group(name, l);
    return *this;
}


metric_definition::metric_definition(impl::metric_definition_impl const& m) :
    _impl(std::make_unique<impl::metric_definition_impl>(m)) {
}

bool label_instance::operator<(const label_instance& id2) const {
    auto& id1 = *this;
    return std::tie(id1.key(), id1.value())
                < std::tie(id2.key(), id2.value());
}

bool label_instance::operator==(const label_instance& id2) const {
    auto& id1 = *this;
    return std::tie(id1.key(), id1.value())
                    == std::tie(id2.key(), id2.value());
}


static std::string get_hostname() {
    char hostname[PATH_MAX];
    gethostname(hostname, sizeof(hostname));
    hostname[PATH_MAX-1] = '\0';
    return hostname;
}


boost::program_options::options_description get_options_description() {
    namespace bpo = boost::program_options;
    bpo::options_description opts("Metrics options");
    opts.add_options()(
            "metrics-hostname",
            bpo::value<std::string>()->default_value(get_hostname()),
            "set the hostname used by the metrics, if not set, the local hostname will be used");
    return opts;
}

future<> configure(const boost::program_options::variables_map & opts) {
    impl::config c;
    c.hostname = opts["metrics-hostname"].as<std::string>();
    return smp::invoke_on_all([c] {
        impl::get_local_impl()->set_config(c);
    });
}


bool label_instance::operator!=(const label_instance& id2) const {
    auto& id1 = *this;
    return !(id1 == id2);
}

label shard_label("shard");
label type_label("type");
namespace impl {

registered_metric::registered_metric(metric_id id, data_type type, metric_function f, description d, bool enabled) :
        _type(type), _d(d), _enabled(enabled), _f(f), _impl(get_local_impl()), _id(id) {
}

metric_value metric_value::operator+(const metric_value& c) {
    metric_value res(*this);
    switch (_type) {
    case data_type::HISTOGRAM:
        boost::get<histogram>(res.u) += boost::get<histogram>(c.u);
    default:
        boost::get<double>(res.u) += boost::get<double>(c.u);
        break;
    }
    return res;
}

metric_definition_impl::metric_definition_impl(
        metric_name_type name,
        metric_type type,
        metric_function f,
        description d,
        std::vector<label_instance> _labels)
        : name(name), type(type), f(f)
        , d(d), enabled(true) {
    for (auto i: _labels) {
        labels[i.key()] = i.value();
    }
    if (labels.find(shard_label.name()) == labels.end()) {
        labels[shard_label.name()] = shard();
    }
    if (labels.find(type_label.name()) == labels.end()) {
        labels[type_label.name()] = type.type_name;
    }
}

metric_definition_impl& metric_definition_impl::operator ()(bool _enabled) {
    enabled = _enabled;
    return *this;
}

metric_definition_impl& metric_definition_impl::operator ()(const label_instance& label) {
    labels[label.key()] = label.value();
    return *this;
}

std::unique_ptr<metric_groups_def> create_metric_groups() {
    return  std::make_unique<metric_groups_impl>();
}

metric_groups_impl::~metric_groups_impl() {
    for (auto i : _registration) {
        unregister_metric(i);
    }
}

metric_groups_impl& metric_groups_impl::add_metric(group_name_type name, const metric_definition& md)  {

    metric_id id(name, md._impl->name, md._impl->labels);

    shared_ptr<registered_metric> rm =
            ::make_shared<registered_metric>(id, md._impl->type.base_type, md._impl->f, md._impl->d, md._impl->enabled);

    get_local_impl()->add_registration(id, rm);

    _registration.push_back(id);
    return *this;
}

metric_groups_impl& metric_groups_impl::add_group(group_name_type name, const std::vector<metric_definition>& l) {
    for (auto i = l.begin(); i != l.end(); ++i) {
        add_metric(name, *(i->_impl.get()));
    }
    return *this;
}

metric_groups_impl& metric_groups_impl::add_group(group_name_type name, const std::initializer_list<metric_definition>& l) {
    for (auto i = l.begin(); i != l.end(); ++i) {
        add_metric(name, *i);
    }
    return *this;
}

bool metric_id::operator<(
        const metric_id& id2) const {
    return as_tuple() < id2.as_tuple();
}

static std::string safe_name(const sstring& name) {
    auto rep = boost::replace_all_copy(boost::replace_all_copy(name, "-", "_"), " ", "_");
    boost::remove_erase_if(rep, boost::is_any_of("+()"));
    return rep;
}

sstring metric_id::full_name() const {
    return safe_name(_group + "_" + _name);
}

bool metric_id::operator==(
        const metric_id & id2) const {
    return as_tuple() < id2.as_tuple();
}

// Unfortunately, metrics_impl can not be shared because it
// need to be available before the first users (reactor) will call it

shared_ptr<impl>  get_local_impl() {
    static thread_local auto the_impl = make_shared<impl>();
    return the_impl;
}

void unregister_metric(const metric_id & id) {
    shared_ptr<impl> map = get_local_impl();
    auto i = map->get_value_map().find(id.full_name());
    if (i != map->get_value_map().end()) {
        auto j = i->second.find(id.labels());
        if (j != i->second.end()) {
            j->second = nullptr;
            i->second.erase(j);
        }
        if (i->second.empty()) {
            map->get_value_map().erase(i);
        }
    }
}

const value_map& get_value_map() {
    return get_local_impl()->get_value_map();
}

values_copy get_values() {
    values_copy res;

    for (auto i : get_local_impl()->get_value_map()) {
        std::vector<std::tuple<shared_ptr<registered_metric>, metric_value>> values;
        for (auto&& v : i.second) {
            if (v.second.get() && v.second->is_enabled()) {
                values.emplace_back(v.second, (*(v.second))());
            }
        }
        if (values.size() > 0) {
            res[i.first] = std::move(values);
        }
    }
    return std::move(res);
}


instance_id_type shard() {
    return to_sstring(engine().cpu_id());
}

void impl::add_registration(const metric_id& id, shared_ptr<registered_metric> rm) {
    sstring name = id.full_name();
    if (_value_map.find(name) != _value_map.end()) {
        auto& metric = _value_map[name];
        if (metric.find(id.labels()) != metric.end()) {
            throw std::runtime_error("registering metrics twice for metrics: " + name);
        }
        if (metric.begin()->second->get_type() != rm->get_type()) {
            throw std::runtime_error("registering metrics " + name + " registered with different type.");
        }
        metric[id.labels()] = rm;
    } else {
        _value_map[name].info().type = rm->get_type();
        _value_map[name][id.labels()] = rm;
    }
}

}

const bool metric_disabled = false;


histogram& histogram::operator+=(const histogram& c) {
    for (size_t i = 0; i < c.buckets.size(); i++) {
        if (buckets.size() <= i) {
            buckets.push_back(c.buckets[i]);
        } else {
            if (buckets[i].upper_bound != c.buckets[i].upper_bound) {
                throw std::out_of_range("Trying to add histogram with different bucket limits");
            }
            buckets[i].count += c.buckets[i].count;
        }
    }
    return *this;
}

histogram histogram::operator+(const histogram& c) const {
    histogram res = *this;
    res += c;
    return res;
}

histogram histogram::operator+(histogram&& c) const {
    c += *this;
    return std::move(c);
}

}
}
