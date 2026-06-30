#pragma once

#include "StarCelestialCoordinate.hpp"
#include "StarWorldParameters.hpp"

namespace Star {

STAR_CLASS(CelestialParameters);

class CelestialParameters {
public:
  CelestialParameters();
  CelestialParameters(CelestialCoordinate coordinate, uint64_t seed, String name, Json parameters);
  explicit CelestialParameters(Json const& diskStore);
  explicit CelestialParameters(ByteArray netStore);

  Json diskStore() const;
  ByteArray netStore() const;

  CelestialCoordinate coordinate() const;
  String name() const;
  uint64_t seed() const;

  Json parameters() const;
  Json getParameter(String const& name, Json def = Json()) const;
  // Predictably select from a json array, given by the named parameter.
  // Selects based on the name hash and the system seed.
  Json randomizeParameterList(String const& name, int32_t mix = 0) const;
  // Predictably select from a range, given by the named parameter.  Works for
  // either floating or integral ranges.
  Json randomizeParameterRange(String const& name, int32_t mix = 0) const;
  // Same function, but if you want to specify the range from an external source
  Json randomizeParameterRange(JsonArray const& range, int32_t mix = 0, Maybe<String> const& name = {}) const;

  // Not all worlds are visitable, if the world is not visitable its
  // visitableParameters will be empty.
  bool isVisitable() const;
  VisitableWorldParametersConstPtr visitableParameters() const;
  void setVisitableParameters(VisitableWorldParametersPtr const& newVisitableParameters);

private:
  // Visitable world parameters (biomes/terrain/dungeons) are expensive to generate
  // and deterministic from (m_parameters, m_seed). They are generated LAZILY on first
  // access instead of in the constructor -- the starter-world search and chunk
  // generation create CelestialParameters for huge numbers of planets but only ever
  // inspect the biome/size of a handful, so eager generation was a massive waste.
  void ensureVisitableParameters() const;

  CelestialCoordinate m_coordinate;
  uint64_t m_seed;
  String m_name;
  Json m_parameters;
  mutable bool m_visitableParametersGenerated = false;
  mutable VisitableWorldParametersConstPtr m_visitableParameters;
};

}
