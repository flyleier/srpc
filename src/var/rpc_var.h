/*
  Copyright (c) 2022 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#ifndef __RPC_VAR_H__
#define __RPC_VAR_H__

#include <utility>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include "time_window_quantiles.h"

namespace srpc
{

class RPCVar;
class RPCVarLocal;
class GaugeVar;
class CounterVar;
class HistogramVar;
class SummaryVar;

enum RPCVarType
{
	VAR_GAUGE		=	0,
	VAR_COUNTER		=	1,
	VAR_HISTOGRAM	=	2,
	VAR_SUMMARY		=	3
};

static std::string type_string(RPCVarType type)
{
	switch (type)
	{
	case VAR_GAUGE:
		return "gauge";
	case VAR_COUNTER:
		return "counter";
	case VAR_HISTOGRAM:
		return "histogram";
	case VAR_SUMMARY:
		return "summary";
	default:
		break;
	}
	return "";
}

class RPCVarFactory
{
public:
	// thread local api
	static GaugeVar *gauge(const std::string& name);

	static CounterVar *counter(const std::string& name);

	static HistogramVar *histogram(const std::string& name);

	static SummaryVar *summary(const std::string& name);

	static RPCVar *var(const std::string& name);
	static bool check_name_format(const std::string& name);
};
class RPCVarGlobal
{
public:
	static RPCVarGlobal *get_instance()
	{
		static RPCVarGlobal kInstance;
		return &kInstance;
	}

	void add(RPCVarLocal *var)
	{
		this->mutex.lock();
		this->local_vars.push_back(var);
		this->mutex.unlock();
	}

	void del(const RPCVarLocal *var);
	RPCVar *find(const std::string& name);
	void dup(const std::unordered_map<std::string, RPCVar *>& vars);

private:
	RPCVarGlobal() { }

public:
	std::mutex mutex;
	std::vector<RPCVarLocal *> local_vars;
//	friend class RPCVarFactory;
};

class RPCVarLocal
{
public:
	static RPCVarLocal *get_instance()
	{
		static thread_local RPCVarLocal kInstance;
		return &kInstance;
	}

	void add(std::string name, RPCVar *var)
	{
		this->mutex.lock();
		const auto it = this->vars.find(name);

		if (it == this->vars.end())
			this->vars.insert(std::make_pair(std::move(name), var));

		this->mutex.unlock();
	}

	virtual ~RPCVarLocal();

private:
	RPCVarLocal()
	{
		RPCVarGlobal::get_instance()->add(this);
	}

public:
	std::mutex mutex;
	std::unordered_map<std::string, RPCVar *> vars;
	friend class RPCVarGlobal;
//	friend class RPCVarFactory;
};

class RPCVarCollector
{
public:
	virtual void collect_gauge(RPCVar *gauge, double data) = 0;

	virtual void collect_counter_each(RPCVar *counter, const std::string& label,
									  double data) = 0;

	virtual void collect_histogram_begin(RPCVar *histogram) = 0;
	virtual void collect_histogram_each(RPCVar *histogram,
										double bucket_boudary,
										size_t current_count) = 0;
	virtual void collect_histogram_end(RPCVar *histogram, double sum,
									   size_t count) = 0;

	virtual void collect_summary_begin(RPCVar *summary) = 0;
	virtual void collect_summary_each(RPCVar *summary, double quantile,
									  double quantile_out) = 0;
	virtual void collect_summary_end(RPCVar *summary, double sum,
									 size_t count) = 0;
};

class RPCVar
{
public:
	const std::string& get_name() const { return this->name; }
	const std::string& get_help() const { return this->help; }
	RPCVarType get_type() const { return this->type; }
	const std::string get_type_str() const { return type_string(this->type); }

	virtual RPCVar *create(bool with_data) = 0;
	virtual bool reduce(const void *ptr, size_t sz) = 0;
	virtual size_t get_size() const = 0;
	virtual const void *get_data() const = 0;
	virtual void collect(RPCVarCollector *collector) = 0;

public:
	RPCVar(const std::string& name, const std::string& help, RPCVarType type) :
		name(name),
		help(help),
		type(type)
	{
//		this->format_name();
	}

	virtual ~RPCVar() {}

private:
	void format_name();

protected:
	std::string name;
	std::string help;
	RPCVarType type;
};

class GaugeVar : public RPCVar
{
public:
	void increase() { ++this->data; }
	void decrease() { --this->data; }
	size_t get_size() const override { return sizeof(double); }
	const void *get_data() const override { return &this->data; }

	virtual double get() const { return this->data; }
	virtual void set(double var) { this->data = var; }

	RPCVar *create(bool with_data) override;

	bool reduce(const void *ptr, size_t sz) override
	{
		this->data += *(double *)ptr;
//		fprintf(stderr, "reduce data=%d *ptr=%d\n", this->data, *(double *)ptr);
		return true;
	}

	void collect(RPCVarCollector *collector) override
	{
		collector->collect_gauge(this, this->data);
	}

public:
	GaugeVar(const std::string& name, const std::string& help) :
		RPCVar(name, help, VAR_GAUGE)
	{
		this->data = 0;
	}

private:
	double data;
};

class CounterVar : public RPCVar
{
public:
	using LABEL_MAP = std::map<std::string, std::string>;
	GaugeVar *add(const LABEL_MAP& labels);

	RPCVar *create(bool with_data) override;
	bool reduce(const void *ptr, size_t sz) override;
	void collect(RPCVarCollector *collector) override;

	size_t get_size() const override { return this->data.size(); }
	const void *get_data() const override { return &this->data; }

	static bool label_to_str(const LABEL_MAP& labels, std::string& str);

public:
	CounterVar(const std::string& name, const std::string& help) :
		RPCVar(name, help, VAR_COUNTER)
	{
	}

	~CounterVar();
private:
	std::unordered_map<std::string, GaugeVar *> data;
};

class HistogramVar : public RPCVar
{
public:
	void observe(double value);

	// multi is the histogram count of each bucket, including +inf
	bool observe_multi(const std::vector<size_t>& multi, double sum);

	RPCVar *create(bool with_data) override;
	bool reduce(const void *ptr, size_t sz) override;
	void collect(RPCVarCollector *collector) override;

	size_t get_size() const override { return this->bucket_counts.size(); }
	const void *get_data() const override { return this; }

	double get_sum() const { return this->sum; }
	size_t get_count() const { return this->count; }
	const std::vector<size_t> *get_bucket_counts() const
	{
		return &this->bucket_counts;
	}

public:
	HistogramVar(const std::string& name, const std::string& help,
				 const std::vector<double>& bucket);

private:
	std::vector<double> bucket_boundaries;
	std::vector<size_t> bucket_counts;
	double sum;
	size_t count;
};

class SummaryVar : public RPCVar
{
public:
	void observe(double value);

	RPCVar *create(bool with_data) override;
	bool reduce(const void *ptr, size_t sz) override;
	void collect(RPCVarCollector *collector) override;

	size_t get_size() const override { return this->quantiles.size(); }
	const void *get_data() const override { return this; }

	double get_sum() const { return this->sum; }
	size_t get_count() const { return this->count; }
	TimeWindowQuantiles<double> *get_quantile_values()
	{
		return &this->quantile_values;
	}

public:
	SummaryVar(const std::string& name, const std::string& help,
			   const std::vector<struct Quantile>& quantile,
			   const std::chrono::milliseconds max_age, int age_bucket);

private:
	std::vector<struct Quantile> quantiles;
	double sum;
	size_t count;
	size_t quantile_size;
	std::chrono::milliseconds max_age;
	int age_buckets;
	TimeWindowQuantiles<double> quantile_values;
	std::vector<double> quantile_out;
};

} // end namespace srpc

#endif

