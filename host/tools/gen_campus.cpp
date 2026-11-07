// Writes the campus collision model: gen_campus [out=container/models/campus/model.sdf]
// Source of truth is shared/scene/campus.hpp; host/tests/test_campus.cpp fails if the committed file is stale.
#include <cstdio>

#include "campus.hpp"

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "container/models/campus/model.sdf";
  std::FILE* f = std::fopen(path, "wb");
  if (!f) { std::perror(path); return 1; }
  const std::string sdf = astrosim::campus::toSdf(astrosim::campus::scene());
  std::fwrite(sdf.data(), 1, sdf.size(), f);
  std::fclose(f);
  return 0;
}
