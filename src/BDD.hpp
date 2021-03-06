#pragma once

#include "truth_table.hpp"

#include <iostream>
#include <vector>
#include <unordered_map>
#include <functional>
#include <utility>
#include <tuple>

/* These are just some hacks to hash std::pair and std::tuple.
 * You don't need to understand this part. */
namespace std
{
template<class T>
inline void hash_combine( size_t& seed, T const& v )
{
  seed ^= hash<T>()(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template<>
struct hash<pair<uint32_t, uint32_t>>
{
  using argument_type = pair<uint32_t, uint32_t>;
  using result_type = size_t;
  result_type operator() ( argument_type const& in ) const
  {
    result_type seed = 0;
    hash_combine( seed, in.first );
    hash_combine( seed, in.second );
    return seed;
  }
};

template<>
struct hash<tuple<uint32_t, uint32_t>>
{
  using argument_type = tuple<uint32_t, uint32_t>;
  using result_type = size_t;
  result_type operator() ( argument_type const& in ) const
  {
    result_type seed = 0;
    hash_combine( seed, std::get<0>( in ) );
    hash_combine( seed, std::get<1>( in ) );
    return seed;
  }
};

template<>
struct hash<tuple<uint32_t, uint32_t, uint32_t>>
{
  using argument_type = tuple<uint32_t, uint32_t, uint32_t>;
  using result_type = size_t;
  result_type operator() ( argument_type const& in ) const
  {
    result_type seed = 0;
    hash_combine( seed, std::get<0>( in ) );
    hash_combine( seed, std::get<1>( in ) );
    hash_combine( seed, std::get<2>( in ) );
    return seed;
  }
};
}

class BDD
{
public:
  using signal_t = uint32_t;
  /* A signal represents an edge pointing to a node. 
   * The first 31 bits store the index of the node, 
   * and the last bit records whether the edge is complemented or not.
   * See below `make_signal`, `get_index` and `is_complemented`. */

  using var_t = uint32_t;
  /* Declare `var_t` as an alias for an unsigned integer.
   * This datatype will be used for representing variables. */

private:
  using index_t = uint32_t;
  /* Declare `index_t` as an alias for an unsigned integer.
   * This is just for easier understanding of the code.
   * This datatype will be used for node indices. */

  struct Node
  {
    var_t v; /* corresponding variable */
    signal_t T; /* signal of THEN child (should not be complemented) */
    signal_t E; /* signal of ELSE child */
  };

  inline signal_t make_signal( index_t index, bool complement = false ) const
  {
    return complement ? ( index << 1 ) + 1 : index << 1;
  }

  inline index_t get_index( signal_t signal ) const
  {
    assert( ( signal >> 1 ) < nodes.size() );
    return signal >> 1;
  }

  inline Node get_node( signal_t signal ) const
  {
    return nodes[get_index( signal )];
  }

  inline bool is_complemented( signal_t signal ) const
  {
    return signal & 0x1;
  }

public:
  explicit BDD( uint32_t num_vars )
    : unique_table( num_vars ), num_invoke_and( 0u ), num_invoke_or( 0u ), 
      num_invoke_xor( 0u ), num_invoke_ite( 0u )
  {
    nodes.emplace_back( Node({num_vars, 0, 0}) ); /* constant 1 */
    refs.emplace_back( 0 );
  }

  /**********************************************************/
  /***************** Basic Building Blocks ******************/
  /**********************************************************/

  uint32_t num_vars() const
  {
    return unique_table.size();
  }

  /* Get the constant signal. */
  signal_t constant( bool value ) const
  {
    return value ? 0:1 ;
  }

  /* Look up (if exist) or build (if not) the node with variable `var`,
   * THEN child `T`, and ELSE child `E`. */
  signal_t unique( var_t var, signal_t T, signal_t E )
  {
    assert( var < num_vars() && "Variables range from 0 to `num_vars - 1`." );
    assert( get_node( T ).v > var && "With static variable order, children can only be below the node." );
    assert( get_node( E ).v > var && "With static variable order, children can only be below the node." );

    /* Reduction rule: Identical children */
    if ( T == E )
    {
      return T;
    }

    bool output_neg = is_complemented(T);
    if(output_neg){
      T = NOT(T);
      E = NOT(E);
    }

    /* Look up in the unique table. */
    const auto it = unique_table[var].find( {T, E} );
    if ( it != unique_table[var].end() )
    {
      /* The required node already exists. Return it. */
      return make_signal( it->second, output_neg );
    }
    else
    {
      ref(T);
      ref(E);
      /* Create a new node and insert it to the unique table. */
      index_t const new_index = nodes.size();
      nodes.emplace_back( Node({var, T, E}) );
      refs.emplace_back( 0 );
      unique_table[var][{T, E}] = new_index;
      return make_signal( new_index, output_neg );
    }
  }

  /* Return a node (represented with its index) of function F = x_var or F = ~x_var. */
  signal_t literal( var_t var, bool complement = false )
  {
    return unique( var, constant( !complement ), constant( complement ) );
  }

  /**********************************************************/
  /*********************** Ref & Deref **********************/
  /**********************************************************/
  signal_t ref( signal_t f )
  {
    ++refs[get_index(f)];
    return f;
  }

  void deref( signal_t f )
  {
    if (f == constant(false) || f == constant(true)) return ; 

    Node const& F = get_node( f );

    if (is_dead(f)){
      deref(F.T);
      deref(F.E);
    }

    if (refs[get_index(f)] > 0) {
    --refs[get_index(f)];
    }
  }
  
  /**********************************************************/
  /********************* BDD Operations *********************/
  /**********************************************************/

  /* Compute ~f */
  signal_t NOT( signal_t f )
  {
    return f^(0x1);
  }

  /* Compute f ^ g */
  signal_t XOR( signal_t f, signal_t g )
  {
    ++num_invoke_xor;

    if (get_index(f) >= get_index(g)) {
      const signal_t buffer = f;
      f = g;
      g = buffer;
    }
    if (is_complemented(f)) { //since XOR(f,g) = XOR(~f, ~g)
      f = NOT(f);
      g = NOT(g);
    }

    bool f_is_complemented = is_complemented(f);
    bool g_is_complemented = is_complemented(g);

    const auto it = computed_table_XOR.find(std::make_tuple(f,g));
    if (it != computed_table_XOR.end()) {
      /* we found a node in the cache */
      return it->second;
    }

    const auto it2 = computed_table_XOR.find(std::make_tuple(g,f));
    if (it2 != computed_table_XOR.end()) {
      /* we found a node in the cache */
      return it2->second;
    }

    Node const& F = get_node( f );
    Node const& G = get_node( g );

    /* trivial cases */
    if ( f == g )
    {
      return constant( false );
    }
    if ( f == constant( false ) )
    {
      return g;
    }
    if ( g == constant( false ) )
    {
      return f;
    }
    if ( f == constant( true ) )
    {
      return NOT( g );
    }
    if ( g == constant( true ) )
    {
      return NOT( f );
    }
    if ( f == NOT( g ) )
    {
      return constant( true );
    }

    var_t x;
    signal_t f0, f1, g0, g1;

    if ( F.v < G.v ) /* F is on top of G */
    {
      x = F.v;
      f0 = F.E;
      f1 = F.T;
      g0 = g1 = g;
    }
    else if ( G.v < F.v ) /* G is on top of F */
    {
      x = G.v;
      f0 = f1 = f;
      g0 = g_is_complemented ? NOT(G.E) : G.E;
      g1 = g_is_complemented ? NOT(G.T) : G.T;
    }
    else /* F and G are at the same level */
    {
      x = F.v;
      f0 = F.E;
      f1 = F.T;
      g0 = g_is_complemented ? NOT(G.E) : G.E;
      g1 = g_is_complemented ? NOT(G.T) : G.T;
    }

    signal_t const r0 = XOR( f0, g0 );
    signal_t const r1 = XOR( f1, g1 );
    signal_t result =  unique( x, r1, r0 );
    
    deref(x);

    computed_table_XOR.emplace(std::make_tuple(f, g) , result);
    return result;
  }

  /* Compute f & g */
  signal_t AND( signal_t f, signal_t g )
  {
    ++num_invoke_and;

    if (get_index(f) >= get_index(g)) {
      const signal_t buffer = f;
      f = g;
      g = buffer;
    }

    bool f_is_complemented = is_complemented(f);
    bool g_is_complemented = is_complemented(g);

    const auto it = computed_table_AND.find(std::make_tuple(f,g));
    if (it != computed_table_AND.end()) {
      /* we found a node in the cache */
      return it -> second;
    }

    const auto it2 = computed_table_AND.find(std::make_tuple(g,f));
    if (it2 != computed_table_AND.end()) {
      /* we found a node in the cache */
      return it2 -> second;
    }

    Node const& F = get_node( f );
    Node const& G = get_node( g );

    /* trivial cases */
    if ( f == constant( false ) || g == constant( false ) )
    {
      return constant( false );
    }
    if ( f == constant( true ) )
    {
      return g;
    }
    if ( g == constant( true ) )
    {
      return f;
    }
    if ( f == g )
    {
      return f;
    }

    var_t x;
    signal_t f0, f1, g0, g1;

    if ( F.v < G.v ) /* F is on top of G */
    {
      x = F.v;
      f0 = f_is_complemented ? NOT(F.E) : F.E;
      f1 = f_is_complemented ? NOT(F.T) : F.T;
      g0 = g1 = g;
    }
    else if ( G.v < F.v ) /* G is on top of F */
    {
      x = G.v;
      f0 = f1 = f;
      g0 = g_is_complemented ? NOT(G.E) : G.E;
      g1 = g_is_complemented ? NOT(G.T) : G.T;
    }
    else /* F and G are at the same level */
    {
      x = F.v;
      f0 = f_is_complemented ? NOT(F.E) : F.E;
      f1 = f_is_complemented ? NOT(F.T) : F.T;
      g0 = g_is_complemented ? NOT(G.E) : G.E;
      g1 = g_is_complemented ? NOT(G.T) : G.T;
    }

    signal_t const r0 = AND( f0, g0 );
    signal_t const r1 = AND( f1, g1 );
    signal_t result =  unique( x, r1, r0 );

    deref(x);

    computed_table_AND.emplace(std::make_tuple(f, g) , result);
    return result;
  }

  /* Compute f | g */
  signal_t OR( signal_t f, signal_t g )
  {
    ++num_invoke_or;

    if (get_index(f) >= get_index(g)) {
      const signal_t buffer = f;
      f = g;
      g = buffer;
    }

    bool f_is_complemented = is_complemented(f);
    bool g_is_complemented = is_complemented(g);

    const auto it = computed_table_OR.find(std::make_tuple(f,g));
    if (it != computed_table_OR.end()) {
      /* we found a node in the cache */
      return it -> second; 
    }

    const auto it2 = computed_table_OR.find(std::make_tuple(g,f));
    if (it2 != computed_table_OR.end()) {
      /* we found a node in the cache */
      return it2 -> second; 
    }

    Node const& F = get_node( f );
    Node const& G = get_node( g );

    /* trivial cases */
    if ( f == constant( true ) || g == constant( true ) )
    {
      return constant( true );
    }
    if ( f == constant( false ) )
    {
      return g;
    }
    if ( g == constant( false ) )
    {
      return f;
    }
    if ( f == g )
    {
      return f;
    }

    var_t x;
    signal_t f0, f1, g0, g1;

    if ( F.v < G.v ) /* F is on top of G */
    {
      x = F.v;
      f0 = f_is_complemented ? NOT(F.E) : F.E;
      f1 = f_is_complemented ? NOT(F.T) : F.T;
      g0 = g1 = g;
    }
    else if ( G.v < F.v ) /* G is on top of F */
    {
      x = G.v;
      f0 = f1 = f;
      g0 = g_is_complemented ? NOT(G.E) : G.E;
      g1 = g_is_complemented ? NOT(G.T) : G.T;
    }
    else /* F and G are at the same level */
    {
      x = F.v;
      f0 = f_is_complemented ? NOT(F.E) : F.E;
      f1 = f_is_complemented ? NOT(F.T) : F.T;
      g0 = g_is_complemented ? NOT(G.E) : G.E;
      g1 = g_is_complemented ? NOT(G.T) : G.T;
    }

    signal_t const r0 = OR( f0, g0 );
    signal_t const r1 = OR( f1, g1 );
    signal_t result =  unique( x, r1, r0 );
    
    deref(x);

    computed_table_OR.emplace(std::make_tuple(f, g) , result);
    return result;
  }

  /* Compute ITE(f, g, h), i.e., f ? g : h */
  signal_t ITE( signal_t f, signal_t g, signal_t h )
  {
    ++num_invoke_ite;

    if (is_complemented(f)) {
      f = NOT(f);
      const signal_t buffer = g;
      g = h;
      h = buffer;
    }

    bool f_is_complemented = is_complemented(f);
    bool g_is_complemented = is_complemented(g);
    bool h_is_complemented = is_complemented(h);

    const auto it = computed_table_ITE.find(std::make_tuple(f,g,h));
    if (it != computed_table_ITE.end()) {
      /* we found a node in the cache */
      return it -> second;
    }

    Node const& F = get_node( f );
    Node const& G = get_node( g );
    Node const& H = get_node( h );

    /* trivial cases */
    if ( f == constant( true ) )
    {
      return g;
    }
    if ( f == constant( false ) )
    {
      return h;
    }
    if ( g == h )
    {
      return g;
    }

    var_t x;
    signal_t f0, f1, g0, g1, h0, h1;

    if ( F.v <= G.v && F.v <= H.v ) /* F is not lower than both G and H */
    {
      x = F.v;
      f0 = F.E;
      f1 = F.T;
      if ( G.v == F.v )
      {
        g0 = g_is_complemented ? NOT(G.E) : G.E;
        g1 = g_is_complemented ? NOT(G.T) : G.T;
      }
      else
      {
        g0 = g1 = g;
      }
      if ( H.v == F.v )
      {
        h0 = h_is_complemented ? NOT(H.E) : H.E;
        h1 = h_is_complemented ? NOT(H.T) : H.T;
      }
      else
      {
        h0 = h1 = h;
      }
    }
    else /* F.v > min(G.v, H.v) */
    {
      f0 = f1 = f;
      if ( G.v < H.v )
      {
        x = G.v;
        g0 = g_is_complemented ? NOT(G.E) : G.E;
        g1 = g_is_complemented ? NOT(G.T) : G.T;
        h0 = h1 = h;
      }
      else if ( H.v < G.v )
      {
        x = H.v;
        g0 = g1 = g;
        h0 = h_is_complemented ? NOT(H.E) : H.E;
        h1 = h_is_complemented ? NOT(H.T) : H.T;
      }
      else /* G.v == H.v */
      {
        x = G.v;
        g0 = g_is_complemented ? NOT(G.E) : G.E;
        g1 = g_is_complemented ? NOT(G.T) : G.T;
        h0 = h_is_complemented ? NOT(H.E) : H.E;
        h1 = h_is_complemented ? NOT(H.T) : H.T;
      }
    }

    signal_t const r0 = ITE( f0, g0, h0 );
    signal_t const r1 = ITE( f1, g1, h1 );
    signal_t result =  unique( x, r1, r0 );
    
    deref(x);

    computed_table_ITE.emplace(std::make_tuple(f, g, h) , result);
    return result;
  }

  /**********************************************************/
  /***************** Printing and Evaluating ****************/
  /**********************************************************/

  /* Print the BDD rooted at node `f`. */
  void print( signal_t f, std::ostream& os = std::cout ) const
  {
    Node const& F = get_node( f );
    for ( auto i = 0u; i < F.v; ++i )
    {
      os << "  ";
    }
    if ( f <= 1 )
    {
      os << "constant " << ( f ? "0" : "1" ) << std::endl;
    }
    else
    {
      if ( is_complemented( f ) )
      {
        os << "!";
      }
      else
      {
        os << " ";
      }

      os << "node " << get_index( f ) << ": var = " << F.v << ", T = " << F.T 
         << ", E = " << F.E << std::endl;
      for ( auto i = 0u; i < F.v; ++i )
      {
        os << "  ";
      }
      os << "> THEN branch" << std::endl;
      print( F.T, os );
      for ( auto i = 0u; i < F.v; ++i )
      {
        os << "  ";
      }
      os << "> ELSE branch" << std::endl;
      print( F.E, os );
    }
  }

  /* Get the truth table of the BDD rooted at node f. */
  Truth_Table get_tt( signal_t f ) const
  {
    Node const& F = get_node( f );

    if ( f == constant( false ) )
    {
      return Truth_Table( num_vars() );
    }
    else if ( f == constant( true ) )
    {
      return ~Truth_Table( num_vars() );
    }
    
    /* Shannon expansion: f = x f_x + x' f_x' */
    var_t const x = F.v;
    signal_t const fx = F.T;
    signal_t const fnx = F.E;
    Truth_Table const tt_x = create_tt_nth_var( num_vars(), x );
    Truth_Table const tt_nx = create_tt_nth_var( num_vars(), x, false );
    if ( is_complemented( f ) )
    {
      return ~( ( tt_x & get_tt( fx ) ) | ( tt_nx & get_tt( fnx ) ) );
    }
    else
    {
      return ( tt_x & get_tt( fx ) ) | ( tt_nx & get_tt( fnx ) );
    }
  }

  /* Whether `f` is dead (having a reference count of 0). */
  bool is_dead( index_t f ) const
  {
    return refs[f] == 0u;
  }

  /* Get the number of living nodes in the whole package, excluding constants. */
  uint64_t num_nodes() const
  {
    uint64_t n = 0u;
    for ( auto i = 1u; i < nodes.size(); ++i )
    {
      if ( !is_dead( i ) )
      {
        ++n;
      }
    }
    return n;
  }

  /* Get the number of nodes in the sub-graph rooted at node f, excluding constants. */
  uint64_t num_nodes( signal_t f ) const
  {
    if ( f == constant( false ) || f == constant( true ) )
    {
      return 0u;
    }

    std::vector<bool> visited( nodes.size(), false );
    visited[0] = true;

    return num_nodes_rec( get_index( f ), visited );
  }

  uint64_t num_invoke() const
  {
    return num_invoke_and + num_invoke_or + num_invoke_xor + num_invoke_ite;
  }

private:
  /**********************************************************/
  /******************** Helper Functions ********************/
  /**********************************************************/

  uint64_t num_nodes_rec( index_t f, std::vector<bool>& visited ) const
  {
    assert( f < nodes.size() && "Make sure f exists." );

    uint64_t n = 0u;
    Node const& F = nodes[f];
    if ( !visited[get_index( F.T )] )
    {
      n += num_nodes_rec( get_index( F.T ), visited );
      visited[get_index( F.T )] = true;
    }
    if ( !visited[get_index( F.E )] )
    {
      n += num_nodes_rec( get_index( F.E ), visited );
      visited[get_index( F.E )] = true;
    }
    return n + 1u;
  }

private:
  std::vector<Node> nodes;
  std::vector<uint32_t> refs; /* The reference counts. Should always be the same size as `nodes`. */
  std::vector<std::unordered_map<std::pair<signal_t, signal_t>, index_t>> unique_table;
  /* `unique_table` is a vector of `num_vars` maps storing the built nodes of each variable.
   * Each map maps from a pair of node indices (T, E) to a node index, if it exists.
   * See the implementation of `unique` for example usage. */

  /* Computed tables for each operation type. */
  std::unordered_map<std::tuple<signal_t, signal_t>, signal_t> computed_table_AND;
  std::unordered_map<std::tuple<signal_t, signal_t>, signal_t> computed_table_OR;
  std::unordered_map<std::tuple<signal_t, signal_t>, signal_t> computed_table_XOR;
  std::unordered_map<std::tuple<signal_t, signal_t, signal_t>, signal_t> computed_table_ITE;

  /* statistics */
  uint64_t num_invoke_and, num_invoke_or, num_invoke_xor, num_invoke_ite;
};
