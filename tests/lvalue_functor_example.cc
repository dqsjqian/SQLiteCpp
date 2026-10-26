#include<iostream>
#include<SQLiteCpp.h>
#include<string>
#include<vector>
using namespace  sqlite;
using namespace std;

template<typename Target, typename... AttrTypes>
struct builder {
    vector<Target> results;

    void operator()(AttrTypes... args) {
      results.emplace_back(std::forward<AttrTypes&&>(args)...);
    };
};


struct user_record {
	int age;
	string name;
	double weight;
	
	user_record(int age_, string name_, double weight_) : age(age_), name(name_), weight(weight_) { }

    static std::vector<user_record> all(sqlite::database& db) {
	  builder<user_record, int, std::string, double> person_builder;
      db << "SELECT * FROM user;"
        >> person_builder;
      return std::move(person_builder.results); // move to avoid copying data ;-)
    };
};

int main() {

	try {
		database db(":memory:");

		db <<
			"create table if not exists user ("
			"   age int,"
			"   name text,"
			"   weight real"
			");";

		db << "insert into user (age,name,weight) values (?,?,?);" << 20 << u"chandler" << 83.25;
		db << "insert into user (age,name,weight) values (?,?,?);" << 21 << u"monika" << 86.25;
		db << "insert into user (age,name,weight) values (?,?,?);" << 22 << u"ross" << 88.25;

		auto users = user_record::all(db);
		
		for(auto u : users)
			cout << u.age << ' ' << u.name << ' ' << u.weight << endl;
        if(users.size() != 3)
			exit(EXIT_FAILURE);
	}
	catch (exception& e) {
		cerr << e.what() << endl;
		exit(EXIT_FAILURE);
	}

  return 0;
}
