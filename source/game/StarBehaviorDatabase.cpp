#include "StarBehaviorDatabase.hpp"
#include "StarAlgorithm.hpp"
#include "StarAssets.hpp"
#include "StarLogging.hpp"
#include "StarRoot.hpp"
#include "StarJsonExtra.hpp"
#include "StarTime.hpp"

namespace Star {

namespace {
  constexpr size_t BehaviorBuildDepthLimit = 2048;
  // Guards against pathologically/recursively expanding trees blowing up
  // memory. This is only a last-resort sanity ceiling; genuine allocation
  // failures are still caught as std::bad_alloc in loadTree(). Mobile uses the
  // same ceiling as desktop: the module sub-tree + cache representation is more
  // memory-efficient than the pre-refactor inline expansion, so any tree that
  // previously built (inlined) on a device will build here too. A lower mobile
  // ceiling was rejecting large modded behavior trees (e.g. FU/Arcane) that
  // build fine, breaking NPC spawning and conversation behaviors.
  constexpr size_t BehaviorBuildNodeLimit = 1000000;

  Json behaviorParameterValueToJson(NodeParameterValue const& value) {
    if (auto key = value.maybe<String>())
      return JsonObject{{"key", *key}};
    else
      return JsonObject{{"value", value.get<Json>()}};
  }

  String behaviorModuleCacheKey(String const& name, StringMap<NodeParameterValue> const& parameters) {
    JsonObject parameterJson;
    for (auto const& parameter : parameters)
      parameterJson.set(parameter.first, behaviorParameterValueToJson(parameter.second));

    return strf("{}\n{}", name, Json(parameterJson).repr(0, true));
  }
}

EnumMap<NodeParameterType> const NodeParameterTypeNames {
  {NodeParameterType::Json, "json"},
  {NodeParameterType::Entity, "entity"},
  {NodeParameterType::Position, "position"},
  {NodeParameterType::Vec2, "vec2"},
  {NodeParameterType::Number, "number"},
  {NodeParameterType::Bool, "bool"},
  {NodeParameterType::List, "list"},
  {NodeParameterType::Table, "table"},
  {NodeParameterType::String, "string"}
};

NodeParameterValue nodeParameterValueFromJson(Json const& json) {
  if (auto key = json.optString("key"))
    return *key;
  else
    return json.get("value");
}

Json jsonFromNodeParameter(NodeParameter const& parameter) {
  JsonObject json {
    {"type", NodeParameterTypeNames.getRight(parameter.first)}
  };
  if (auto key = parameter.second.maybe<String>())
    json.set("key", *key);
  else
    json.set("value", parameter.second.get<Json>());
  return json;
}

NodeParameter jsonToNodeParameter(Json const& json) {
  NodeParameterType type = NodeParameterTypeNames.getLeft(json.getString("type"));
  if (auto key = json.optString("key"))
    return {type, *key};
  else
    return {type, json.opt("value").value(Json())};
}

Json nodeOutputToJson(NodeOutput const& output) {
  return JsonObject {
    {"type", NodeParameterTypeNames.getRight(output.first)},
    {"key", jsonFromMaybe<String>(output.second.first, [](String const& s) { return Json(s); })},
    {"ephemeral", output.second.second}
  };
}

NodeOutput jsonToNodeOutput(Json const& json) {
  return {
    NodeParameterTypeNames.getLeft(json.getString("type")),
    {jsonToMaybe<String>(json.get("key"), [](Json const& j) { return j.toString(); }), json.optBool("ephemeral").value(false)}
  };
}

EnumMap<BehaviorNodeType> const BehaviorNodeTypeNames {
  {BehaviorNodeType::Action, "Action"},
  {BehaviorNodeType::Decorator, "Decorator"},
  {BehaviorNodeType::Composite, "Composite"},
  {BehaviorNodeType::Module, "Module"}
};

EnumMap<CompositeType> const CompositeTypeNames {
  {CompositeType::Sequence, "Sequence"},
  {CompositeType::Selector, "Selector"},
  {CompositeType::Parallel, "Parallel"},
  {CompositeType::Dynamic, "Dynamic"},
  {CompositeType::Randomize, "Randomize"}
};

void applyTreeParameters(StringMap<NodeParameter>& nodeParameters, StringMap<NodeParameterValue> const& treeParameters) {
  for (auto& p : nodeParameters) {
    NodeParameter& parameter = p.second;
    parameter.second = replaceBehaviorTag(parameter.second, treeParameters);
  }
}

NodeParameterValue replaceBehaviorTag(NodeParameterValue const& parameter, StringMap<NodeParameterValue> const& treeParameters) {
  Maybe<String> strVal = parameter.maybe<String>();
  if (!strVal && parameter.get<Json>().isType(Json::Type::String))
    strVal = parameter.get<Json>().toString();
  if (strVal) {
    String key = *strVal;
    if (key.beginsWith('<') && key.endsWith('>')) {
      String treeKey = key.substr(1, key.size() - 2);

      if (auto replace = treeParameters.maybe(treeKey)) {
        return *replace;
      } else {
        throw StarException(strf("No parameter specified for tag '{}'", key));
      }
    }
  }
  return parameter;
}

Maybe<String> replaceOutputBehaviorTag(Maybe<String> const& output, StringMap<NodeParameterValue> const& treeParameters) {
  if (auto out = output) {
    if (out->beginsWith('<') && out->endsWith('>')) {
      if (auto replace = treeParameters.maybe(out->substr(1, out->size() - 2))) {
        if (auto key = replace->maybe<String>())
          return *key;
        else if (replace->get<Json>().isType(Json::Type::String))
          return replace->get<Json>().toString();
        else
          return {};
      } else {
        throw StarException(strf("No parameter specified for tag '{}'", *out));
      }
    }
  }
  return output;
}

// TODO: This is temporary until BehaviorState can handle valueType:value pairs
void parseNodeParameters(JsonObject& parameters) {
  for (auto& p : parameters)
    p.second = p.second.opt("key").orMaybe(p.second.opt("value")).value(Json());
}

ActionNode::ActionNode(String name, StringMap<NodeParameter> parameters, StringMap<NodeOutput> output)
  : name(std::move(name)), parameters(std::move(parameters)), output(std::move(output)) { }

DecoratorNode::DecoratorNode(String const& name, StringMap<NodeParameter> parameters, BehaviorNodeConstPtr child)
  : name(name), parameters(parameters), child(child) { }

SequenceNode::SequenceNode(List<BehaviorNodeConstPtr> children) : children(children) { }

SelectorNode::SelectorNode(List<BehaviorNodeConstPtr> children) : children(children) { }

ParallelNode::ParallelNode(StringMap<NodeParameter> parameters, List<BehaviorNodeConstPtr> children) : children(children) {
  int s = parameters.get("success").second.get<Json>().optInt().value(-1);
  succeed = s == -1 ? children.size() : s;

  int f = parameters.get("fail").second.get<Json>().optInt().value(-1);
  fail = f == -1 ? children.size() : f;
}

DynamicNode::DynamicNode(List<BehaviorNodeConstPtr> children) : children(children) { }

RandomizeNode::RandomizeNode(List<BehaviorNodeConstPtr> children) : children(children) { }

BehaviorTree::BehaviorTree(String const& name, StringSet scripts, JsonObject const& parameters)
  : name(name), scripts(scripts), parameters(parameters) { }

struct BehaviorDatabase::BuildContext {
  size_t depth = 0;
  size_t maxDepth = 0;
  size_t nodeCount = 0;
  size_t moduleBuilds = 0;
  size_t moduleCacheHits = 0;
  StringMap<BehaviorTreeConstPtr> moduleCache;
  StringList moduleStack;
};

BehaviorDatabase::BehaviorDatabase() {
  auto assets = Root::singleton().assets();

  auto& nodeFiles = assets->scanExtension("nodes");
  assets->queueJsons(nodeFiles);
  for (String const& file : nodeFiles) {
    try {
      Json nodes = assets->json(file);
      for (auto& node : nodes.toObject()) {
        StringMap<NodeParameter> parameters;
        for (auto p : node.second.getObject("properties", {}))
          parameters.set(p.first, jsonToNodeParameter(p.second));

        m_nodeParameters.set(node.first, parameters);

        StringMap<NodeOutput> output;
        for (auto p : node.second.getObject("output", {}))
          output.set(p.first, jsonToNodeOutput(p.second));

        m_nodeOutput.set(node.first, output);
      }
    } catch (StarException const& e) {
      throw StarException(strf("Could not load nodes file \'{}\'", file), e);
    }
  }

  auto& behaviorFiles = assets->scanExtension("behavior");
  assets->queueJsons(behaviorFiles);
  for (auto const& file : behaviorFiles) {
    try {
      auto config = assets->json(file);
      auto name = config.getString("name");

      if (m_configs.contains(name))
        throw StarException(strf("Duplicate behavior tree \'{}\'", name));

      m_configs[name] = config;
    } catch (StarException const& e) {
      throw StarException(strf("Could not load behavior file \'{}\'", file), e);
    }
  }

  Logger::info("BehaviorDatabase: Scanned {} node file(s), {} behavior file(s), {} named behavior tree(s)",
      nodeFiles.size(), behaviorFiles.size(), m_configs.size());

// Gated on the real mobile-device family, not STAR_PLATFORM_MOBILE: a desktop
// build using the oSBM launcher path keeps the desktop eager behavior-tree
// load; only real mobile devices lazy-load to save memory/startup time.
#if !STAR_SYSTEM_FAMILY_MOBILE
  for (auto& pair : m_configs)
    loadTree(pair.first);
#endif
}

BehaviorTreeConstPtr BehaviorDatabase::behaviorTree(String const& name) const {
  {
    MutexLocker locker(m_behaviorsMutex);
    if (auto behavior = m_behaviors.maybe(name))
      return *behavior;
  }

  return loadTree(name);
}

BehaviorTreeConstPtr BehaviorDatabase::buildTree(Json const& config, StringMap<NodeParameterValue> const& overrides) const {
  BuildContext context;
  return buildTree(config, overrides, context);
}

BehaviorTreeConstPtr BehaviorDatabase::buildTree(Json const& config, StringMap<NodeParameterValue> const& overrides, BuildContext& context) const {
  if (++context.depth > BehaviorBuildDepthLimit)
    throw MemoryException(strf("Behavior tree '{}' exceeded maximum module nesting depth {}", config.getString("name"), BehaviorBuildDepthLimit));
  auto decrementDepth = finally([&context]() { --context.depth; });
  context.maxDepth = max(context.maxDepth, context.depth);

  StringSet scripts = jsonToStringSet(config.get("scripts", JsonArray()));
  auto tree = BehaviorTree(config.getString("name"), scripts, config.getObject("parameters", {}));

  StringMap<NodeParameterValue> parameters;
  for (auto p : config.getObject("parameters", {}))
    parameters.set(p.first, p.second);
  for (auto p : overrides)
    parameters.set(p.first, p.second);
  BehaviorNodeConstPtr root = behaviorNode(config.get("root"), parameters, tree, context);
  tree.root = root;
  return std::make_shared<BehaviorTree>(std::move(tree));
}

Json BehaviorDatabase::behaviorConfig(String const& name) const {
  if (!m_configs.contains(name))
    throw StarException(strf("No such behavior tree \'{}\'", name));

  return m_configs.get(name);
}

BehaviorTreeConstPtr BehaviorDatabase::loadTree(String const& name) const {
  if (!m_configs.contains(name))
    throw StarException(strf("No such behavior tree \'{}\'", name));

  try {
    BuildContext context;
    Logger::info("BehaviorDatabase: Building behavior tree '{}'...", name);
    auto startSeconds = Time::monotonicTime();
    auto behavior = buildTree(m_configs.get(name), {}, context);

    MutexLocker locker(m_behaviorsMutex);
    if (auto existing = m_behaviors.maybe(name))
      return *existing;

    m_behaviors.set(name, behavior);
    Logger::info(
        "BehaviorDatabase: Built '{}' in {} seconds (nodes={}, modulesBuilt={}, moduleCacheHits={}, uniqueModules={}, maxDepth={})",
        name, Time::monotonicTime() - startSeconds, context.nodeCount, context.moduleBuilds, context.moduleCacheHits,
        context.moduleCache.size(), context.maxDepth);
    return behavior;
  } catch (std::bad_alloc const& e) {
    Logger::error("BehaviorDatabase: Out of memory while building behavior tree '{}'", name);
    throw MemoryException(strf("Out of memory while building behavior tree '{}'", name), e);
  } catch (StarException const& e) {
    Logger::error("BehaviorDatabase: Failed building behavior tree '{}': {}", name, outputException(e, false));
    throw StarException(strf("Could not build behavior tree '{}'", name), e);
  }
}

CompositeNode BehaviorDatabase::compositeNode(Json const& config, StringMap<NodeParameter> parameters, StringMap<NodeParameterValue> const& treeParameters, BehaviorTree& tree, BuildContext& context) const {
  List<BehaviorNodeConstPtr> children = config.getArray("children", {}).transformed([this,treeParameters,&tree,&context](Json const& child) {
      return behaviorNode(child, treeParameters, tree, context);
    });

  CompositeType type = CompositeTypeNames.getLeft(config.getString("name"));
  if (type == CompositeType::Sequence)
    return SequenceNode(children);
  else if (type == CompositeType::Selector)
    return SelectorNode(children);
  else if (type == CompositeType::Parallel)
    return ParallelNode(parameters, children);
  else if (type == CompositeType::Dynamic)
    return DynamicNode(children);
  else if (type == CompositeType::Randomize)
    return RandomizeNode(children);

  // above statement needs to be exhaustive
  throw StarException(strf("Composite node type '{}' could not be created from JSON", CompositeTypeNames.getRight(type)));
}

BehaviorNodeConstPtr BehaviorDatabase::behaviorNode(Json const& json, StringMap<NodeParameterValue> const& treeParameters, BehaviorTree& tree, BuildContext& context) const {
  if (++context.nodeCount > BehaviorBuildNodeLimit)
    throw MemoryException(strf("Behavior tree '{}' exceeded maximum expanded node count {}", tree.name, BehaviorBuildNodeLimit));

  BehaviorNodeType type = BehaviorNodeTypeNames.getLeft(json.getString("type"));

  auto name = json.getString("name");
  auto parameterConfig = json.getObject("parameters", {});

  if (type == BehaviorNodeType::Module) {
      // merge in module parameters to a copy of the treeParameters to propagate
      // tree parameters into the sub-tree, but allow modules to override
      auto moduleParameters = treeParameters;
      for (auto p : parameterConfig)
        moduleParameters.set(p.first, replaceBehaviorTag(nodeParameterValueFromJson(p.second), treeParameters));

      auto cacheKey = behaviorModuleCacheKey(name, moduleParameters);
      if (auto module = context.moduleCache.maybe(cacheKey)) {
        ++context.moduleCacheHits;
        // Modules execute as sub-trees, but their scripts and functions must
        // still be propagated to the owning tree so that the master
        // BehaviorState requires every script and resolves every function up
        // front when the behavior is created (e.g. on NPC spawn). Otherwise
        // module script side-effects (message handlers, converse/dialogue
        // setup, init hooks) would be deferred until the module node first
        // runs, causing skipped dialogue and mis-initialized entities.
        tree.scripts.addAll((*module)->scripts);
        tree.functions.addAll((*module)->functions);
        return make_shared<BehaviorNode>(*module);
      }

      if (context.moduleStack.contains(cacheKey))
        throw StarException(strf("Recursive behavior module reference while building '{}' through module '{}'", tree.name, name));

      context.moduleStack.append(cacheKey);
      ++context.moduleBuilds;
      auto module = buildTree(m_configs.get(name), moduleParameters, context);
      context.moduleStack.takeLast();

      context.moduleCache.set(cacheKey, module);

      // Propagate the (transitively collected) scripts and functions of the
      // module up into the owning tree so they are loaded eagerly with the
      // master BehaviorState, matching pre-module-refactor behavior.
      tree.scripts.addAll(module->scripts);
      tree.functions.addAll(module->functions);

      return make_shared<BehaviorNode>(module);
  }

  StringMap<NodeParameter> parameters = m_nodeParameters.get(name);
  for (auto& p : parameters)
    p.second.second = parameterConfig.maybe(p.first).apply(nodeParameterValueFromJson).value(p.second.second);
  applyTreeParameters(parameters, treeParameters);

  if (type == BehaviorNodeType::Action) {
    tree.functions.add(name);

    Json outputConfig = json.getObject("output", {});
    StringMap<NodeOutput> output = m_nodeOutput.get(name);
    for (auto& p : output)
      p.second.second.first = replaceOutputBehaviorTag(outputConfig.optString(p.first).orMaybe(p.second.second.first), treeParameters);

    return make_shared<BehaviorNode>(ActionNode(name, parameters, output));
  } else if (type == BehaviorNodeType::Decorator) {
    tree.functions.add(name);
    BehaviorNodeConstPtr child = behaviorNode(json.get("child"), treeParameters, tree, context);
    return make_shared<BehaviorNode>(DecoratorNode(name, parameters, child));
  } else if (type == BehaviorNodeType::Composite) {
    return make_shared<BehaviorNode>(compositeNode(json, parameters, treeParameters, tree, context));
  }

  // above statement must be exhaustive
  throw StarException(strf("Behavior node type '{}' could not be created from JSON", BehaviorNodeTypeNames.getRight(type)));
}

}
