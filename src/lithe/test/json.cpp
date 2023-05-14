// parsing rules
#include "lithe.h"

// value producers
#include "adapter.h"

// some predefined rules
#include "grammar/common.h"
#include "grammar/json.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>

/*
	First, we construct the grammar for decoding JSON into an abstract
	syntax tree. While we would usually prefer to deserialize/serialize
	to and from native C++ data types, this serves to demonstrate lithe's
	AST capability.
*/

lithe::rule json_ast() {
	using namespace lithe;

	// this matches spaces, line breaks, tabulators etc.
	// because JSON whitespace is ignificant, we drop it by wrapping it in
	// ignore I() directive.
	auto space = I(grammar::common::whitespace());

	// JSON values may contain arrays or objects that contain further values.
	// This is why we need a recursive definition; value is forward-declared
	// and assigned later once all required rules are in place.
	auto value = recursive();

	// use the term generator to insert tagged nodes into the AST. When traversing
	// the AST, the tags will indicate which rule the node came from.
	auto string = E("string", grammar::json::string());

	// grammar::json::string/number() contain lithe parsing rules! check the source
	// code to see how it works.
	auto number = E("number", grammar::json::number());

	// this is a simple rule that matches one of two alternative T() literals.
	auto boolean = E("boolean", T("true") | T("false"));

	// because 'null' has just a single possible literal, we omit the
	// source text "null" by using the require-but-ignore I() literal.
	auto null = E("null", I("null"));

	// use the lithe for_-loop to parse arrays
	auto array = E("array", I("[") << O(space) << // array starts with '['
		for_(
			value  << O(space),   // consume the value rule repeatedly
			I(",") << O(space),   // in between, expect and drop a comma
			I("]"))); // matching ']' indicates end of for_-loop

	// it's best to put the whitespace rules at the end like above;
	// if all rules start similarly and match the whitespace equally well,
	// the loop is going to do a lot of backtracking. By having the 
	// rules diverge from start allows early mismatch detection.

	// the map parser is similar, we just have keys and values instead of
	// plain values.
	auto keyval = E("", string << O(space) << I(":") << O(space) << value);

	auto object = E("object", I("{") << O(space) <<
		for_(
			keyval << O(space),
			I(",") << O(space),
			I("}")));

	// all value types have been defined. we can finalize the recursive rule
	value->assign(string | number | boolean | null | array | object);

	// and that is how we parse JSON
	return O(space) << value << O(space) << end();
}

/*

First, represent JSON types in C++.
We use STL containers and language primitive values for simplicity.

lithe provides a sum_type that can dynamically represent any number
of types. That comes in handy as the containers in JSON are heterogeneous.

*/

namespace json {
	// estabilish primitive data types
	using string = std::string;
	using boolean = bool;
	using number = double;
	using null = nullptr_t;

	// forward declare arrays and objects for the value sum type
	struct array;
	struct object;

	// this type can go into arrays and object property values
	using value = lithe::sum_type<object, array, string, boolean, number, null>;

	// define arrays and objects as subtypes of vector and unordered_map
	struct array : std::vector<value> {
		// constructor just delegates to vector
		template <typename... TArgs>
		array(TArgs&&... args) : vector(std::forward<TArgs>(args)...) {}
	};

	struct object : std::unordered_map<std::string, value> {
		// constructor just delegates to unordered_map
		template <typename... TArgs>
		object(TArgs&&... args) : unordered_map(std::forward<TArgs>(args)...) {}
	};
}

/*

	Finally, we write parsing and lexing logic that will turn
	JSON stream into the C++ types above.

*/

auto json_reader() {
	using namespace lithe;
	using namespace lithe::producers;

	// upon matching the lithe parsing rule grammar::json::string()
	// we produce a native value using the supplied decoding function.
	auto string = convert(grammar::json::string(), grammar::json::decode_string);

	// for numbers we use the C runtime library
	auto number = convert(grammar::json::number(), [](node n) -> json::number {
		auto str = n.get_string();
		return strtod(str.data(), nullptr);
	});

	auto true_  = convert(T("true"),  [](node) -> json::boolean { return true;  });
	auto false_ = convert(T("false"), [](node) -> json::boolean { return false; });
	auto null_ = convert(T("null"), [](node) -> json::null { return nullptr; });

	// recursive value type is needed to represent nested containers
	auto value = producers::recursive<json::value>();

	// containers!
	auto skip_space = O(grammar::common::whitespace());

	auto array = coll_of<json::array>(
		T("[") << skip_space,
			back_emplacer<json::array>(), // how to construct elements into the collection
			value, // this provides the value passed to the emplacer
			skip_space << I(",") << skip_space,  // between values
		skip_space << T("]"));

	auto keyval = continuation(string, skip(skip_space << T(":") << skip_space), value);

	auto object = coll_of<json::object>(
		T("{") << skip_space,
			assoc_emplacer<json::object>(), // how to construct elements into the collection
			keyval, // this provides the value passed to the emplacer
			skip_space << I(",") << skip_space,  // between values
		skip_space << T("}"));

	// complete the definition of value
	value.assign(any(string, number, true_, false_, null_, array, object));
	return value;
}

/*
	We can write serialization by using lithe::sum_type's dispatch and reification.		
*/

namespace json {
	void serialize(std::ostream&, const json::value&);

	void serialize(std::ostream& s, json::number n) {
		s << n;
	}

	void serialize(std::ostream& s, const json::string& str) {
		s << "\"" << lithe::grammar::json::encode_string(str) << "\"";
	}

	void serialize(std::ostream& s, json::boolean b) {
		s << (b ? "true" : "false");
	}

	void serialize(std::ostream& s, json::null) {
		s << "null";
	}

	void serialize(std::ostream& s, const json::array& a) {
		s << "[";
		for (int i = 0;i < a.size();++i) {
			if (i) s << ", ";
			serialize(s, a[i]);
		}
		s << "]";
	}

	void serialize(std::ostream& s, const json::object& a) {
		static thread_local std::string ind;

		s << "{";
		ind.push_back('\t');

		bool first = true;
		for (auto &kv : a) {
			if (first) {
				s << "\n"; first = false;
			} else {
				s << ",\n"; 
			}
			s << ind;
			serialize(s, kv.first);
			s << ": ";
			serialize(s, kv.second);
		}
		ind.pop_back();
		s << "\n" << ind << "}";
	}

	void serialize(std::ostream& s, const json::value& v) {
		// this will dispatch polymorphically based on runtime type.
		v.dispatch([&](auto reified) {
			// 'reified' will be of the type that's currently in 'v'
			serialize(s, reified);
		});
	}

	void serialize(std::ostream& s, lithe::parse_error pe) {
		// 'serializing' parse errors is a horrible idea,
		// but for the purposes of this toy program it allows
		// us to easily see error messages on the screen.
		s << pe;
	}
}


int main() {
	std::string json = "{\"key\": \"value\", \"array\":[1e-10, 2.5, .3 , 4e-15, 5.32143E10], \"boolean\": false, \"map\": {\"thing\": \"ping\"}}";

	auto parser = json_ast();
	auto reader = json_reader();

	auto ast = parser->parse(json);
	std::cout << "AST: "; ast.to_stream(std::cout);

	std::cout << "\n\n";

	lithe::producers::reader(reader, json).dispatch([](auto result) {
		json::serialize(std::cout, result);
	});
}