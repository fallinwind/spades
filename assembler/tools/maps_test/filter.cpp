#include <iostream>
#include <cstdlib>
#include "seq_filter_stat.hpp"
#include <map>
#include <ext/hash_map>
#include <tr1/unordered_map>
#include <google/sparse_hash_map>
#include "cuckoo.hpp"

#include <string>
#include <fstream>

#define K 31

typedef std::map<Seq<K>, size_t, Seq<K>::less2> hm1;
typedef __gnu_cxx::hash_map<Seq<K>, size_t, 
                            Seq<K>::hash, Seq<K>::equal_to> hm2;
typedef std::tr1::unordered_map<Seq<K>, size_t, 
                                Seq<K>::hash, Seq<K>::equal_to> hm3;
typedef google::sparse_hash_map<Seq<K>, size_t, 
                                Seq<K>::hash, Seq<K>::equal_to> hm4;
typedef cuckoo<Seq<K>, size_t, Seq<K>::multiple_hash, Seq<K>::equal_to> hm5; 

int main(int argc, char** argv) {
  std::string filename = "";
  size_t m_num = 1;

  if (argc != 3) {
		std::cout << "Usage: ./filter <filename> <map number>\n"
              << "<map number> is map type:\n"
              << "1 - map, 2 - ext/hash_map, 3 - tr1/unordered_map,\n"
              << "4 - google/sparse_hash_map, 5 - cuckoo\n"
              << "Selects k-mers with amount > 1" << std::endl;
    return 0;
  } else {
		filename = argv[1];
    m_num = atoi(argv[2]);
  }
  switch (m_num) {
  case 1: {
    seq_filter_stat<K, hm1>::filter(filename, "map");
    break;
  }
  case 2: { 
    seq_filter_stat<K, hm2>::filter(filename, "hash_map");
    break;
  }
  case 3: { 
    seq_filter_stat<K, hm3>::filter(filename, "unordered_map");
    break;
  }
  case 4: {
    seq_filter_stat<K, hm4>::filter(filename, "sparse_hash_map");
    break;
  }
  case 5: { 
    seq_filter_stat<K, hm5>::filter(filename, "cuckoo");
    break;
  }
    /*case 6: { //for internal test only!!!
    cuckoo<Seq<28>, size_t, Seq<28>::multiple_hash, Seq<28>::equal_to> map;
    std::fstream f;
    std::string s;
    f.open("/home/mariya/test.utf8", std::fstream::out|std::fstream::in);
    while (!f.eof()) {
      f >> s;
      if (s.length() == 28) {
        Seq<28> seq(s);
        map.insert(std::make_pair<Seq<28>, size_t>(seq, 0));
      }
    }
    f.close();
    std::cout << "OK! " << map.size() << std::endl;
    break; 
    }*/
  default:
    std::cout << "Map number is incorrect!\n";
  }

  return 0;
}
