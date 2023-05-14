#pragma once

#include <list>
#include <string>
#include <cstring>
#include <cstdlib>
#include <ostream>
#include <cassert>
#include <stdexcept>
#include <memory>

namespace CmdLine {
	class IOptionParser {
	public:
		virtual bool Parse(std::list<const char*>& options) = 0;
		virtual const std::string& GetLabel() const = 0;
		virtual const std::string& GetDescription() const = 0;
		virtual const std::string& GetShort() const = 0;
		virtual const std::string& GetLong() const = 0;
		virtual void SetDefault(const std::string&) = 0;
        virtual ~IOptionParser();
	};

	class IRegistry {
	public:
		virtual void Add(IOptionParser&) = 0;
		virtual const char* Parse(std::list<const char*>& options) = 0;
		virtual void ShowHelp(std::ostream& s, const char *banner) = 0;
		virtual void AddParsersTo(IRegistry& other) = 0;
        virtual ~IRegistry();
	};

	IRegistry& Registry();
	std::unique_ptr<IRegistry> NewRegistry();

    template <typename U> static void parameter_exists(U& value) {};
    template <> void parameter_exists<bool>(bool &value) { value = true; }

	template <typename T, bool ADD_REGISTRY = true> class Option : public IOptionParser {
		T value, initial;
		bool Parse(std::list<const char*>& options) override {
			if (options.size()) {
				if (strcmp(options.front(), short_sw.c_str()) == 0 ||
					strcmp(options.front(), long_sw.c_str()) == 0) {
					options.pop_front();

					int count = num_params(T());
                    parameter_exists(value);
                    
					while (count--) {
						if (options.empty()) return false;
						convert(value, options.front());
						options.pop_front();
					}
					return true;
				}
			}
			return false;
		}

		void SetDefault(const std::string& v) override {
			parameter_exists(value);
			convert(value, v.c_str());
		}

		int num_params(std::string) { return 1; }
		int num_params(int) { return 1; }
		int num_params(bool) { return 0; }
		int num_params(std::list<std::string>) { return 1; }

		void convert(bool& v, const char*val) { assert(0 && "bool param shouldn't have a conversion"); }
		void convert(int& v, const char *val) { v = atoi(val); }
		void convert(std::string& v, const char *val) { v = val; }
		void convert(std::list<std::string>& v, const char *val) { v.emplace_back(val); }

		std::string short_sw, long_sw, label, descr;
	public:
		Option(T default_value, const char *l, const char *s, const char *label, const char *descr, IRegistry* registry = &Registry()) {
			Init(default_value, l, s, label, descr, registry);
		}

		Option(const Option&) = delete;
		void operator=(Option) = delete;

		Option() { }

		void Init(T default_value, const char *l, const char *s, const char *label, const char *descr, IRegistry* registry = &Registry()) {
			this->value = default_value;
			this->label = label;
			this->descr = descr;
			long_sw = l;
			short_sw = s;
			initial = value;
			if (registry) registry->Add(*this);
			for (auto &c : long_sw) if (c == '_') c = '-';
		}

		const T& Get() const { return value; }
		const T& operator()() const { return Get(); }
		T& operator()() { return value; }
		const std::string& GetLabel() const override { return label; }
		const std::string& GetDescription() const override { return descr; }
		const std::string& GetShort() const override { return short_sw; }
		const std::string& GetLong() const override { return long_sw; }

		const T& operator=(const T& src) { value = src; return value; }
	};

	template class Option<int>;
	template class Option<std::string>;
	template class Option<bool>;
	template class Option<std::list<std::string>>;
}
