// ==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "simulationcraft.hpp"

namespace
{
struct expiration_t : public event_t
{
  buff_t* buff;

  expiration_t( sim_t* sim, player_t* p, buff_t* b, timespan_t d ) : event_t( sim, p, b -> name_str.c_str() ), buff( b )
  { sim -> add_event( this, d ); }

  virtual void execute()
  {
    buff -> expiration = 0;
    buff -> expire();
  }
};

struct buff_delay_t : public event_t
{
  double     value;
  buff_t*    buff;
  int        stacks;
  timespan_t duration;

  buff_delay_t( sim_t* sim, player_t* p, buff_t* b, int stacks, double value, const timespan_t& d ) :
    event_t( sim, p, b -> name_str.c_str() ), value( value ), buff( b ), stacks( stacks ), duration( d )
  {
    timespan_t delay_duration = sim -> gauss( sim -> default_aura_delay, sim -> default_aura_delay_stddev );
    sim -> add_event( this, delay_duration );
  }

  virtual void execute()
  {
    // Add a Cooldown check here to avoid extra processing due to delays
    if ( buff -> cooldown -> remains() ==  timespan_t::zero() )
      buff -> execute( stacks, value );
    buff -> delay = 0;
  }
};
}

buff_t::buff_t( const buff_creator_t& params ) :
  sim( params._sim ),
  player( params._player.target ),
  name_str( params._name ),
  s_data( params.s_data ),
  _max_stack( 1 ),
  default_value(),
  activated( true ),
  reverse(),
  constant(),
  quiet(),
  overridden(),
  current_value(),
  current_stack(),
  buff_duration( timespan_t() ),
  buff_cooldown( timespan_t() ),
  default_chance( 1.0 ),
  last_start( timespan_t() ),
  last_trigger( timespan_t() ),
  iteration_uptime_sum( timespan_t() ),
  up_count(),
  down_count(),
  start_count(),
  refresh_count(),
  trigger_attempts(),
  trigger_successes(),
  benefit_pct(),
  trigger_pct(),
  avg_start(),
  avg_refresh(),
  source( params._player.source ),
  initial_source( params._player.source ),
  expiration(),
  delay(),
  rng(),
  cooldown(),
  uptime_pct(),
  start_intervals(),
  trigger_intervals()
{
  // Set Buff duration
  if ( params._duration == timespan_t::min() )
  {
    if ( data().ok() )
      buff_duration = data().duration();
  }
  else
    buff_duration = params._duration;

  // Set Buff Cooldown
  if ( params._cooldown == timespan_t::min() )
  {
    if ( data().ok() )
      buff_cooldown = data().cooldown();
  }
  else
    buff_cooldown = params._cooldown;

  // Set Max stacks
  if ( params._max_stack == -1 )
  {
    if ( data().ok() )
    {
      if ( data().max_stacks() != 0 )
        _max_stack = data().max_stacks();
      else if ( data().initial_stacks() != 0 )
        _max_stack = data().initial_stacks();
    }
  }
  else
    _max_stack = params._max_stack;

  // Set Proc Chance
  if ( params._chance == -1.0 )
  {
    if ( data().ok() )
      if ( data().proc_chance() != 0 )
        default_chance = data().proc_chance();
  }
  else
    default_chance = params._chance;

  if ( params._default_value == -1.0 )
  {
    default_value = 0.0;  
  }
  else
    default_value = params._default_value;

  // Set Reverse flag
  if ( params._reverse != -1 )
    reverse = params._reverse != 0;

  // Set Quiet flag
  if ( params._quiet != -1 )
    quiet = params._quiet != 0;

  // Set Activated flag
  if ( params._activated != -1 )
    quiet = params._activated != 0;

  if ( initial_source ) // Player Buffs
  {
    player -> buff_list.push_back( this );
  }
  else // Sim Buffs
  {
    sim -> buff_list.push_back( this );
  }

  init();

}

// buff_t::init =============================================================

void buff_t::init()
{
  if ( _max_stack < 1 )
  {
    _max_stack = 1;
    sim->errorf( "buff %s: initialized with max_stack < 1. Setting max_stack to 1.", name_str.c_str() );
    assert( 0 );
  }

  if ( _max_stack > 999 )
  {
    _max_stack = 999;
    sim->errorf( "buff %s: initialized with max_stack > 999. Setting max_stack to 999.", name_str.c_str() );
  }

  // Keep non hidden reported numbers clean
  start_intervals.mean = 0;
  trigger_intervals.mean = 0;

  buff_duration = std::min( buff_duration, timespan_t::from_seconds( sim -> wheel_seconds - 2.0 ) );


  stack_occurrence.resize( _max_stack + 1 );
  stack_react_time.resize( _max_stack + 1 );

  if ( static_cast<int>( stack_uptime.size() ) < _max_stack )
    for ( int i = static_cast<int>( stack_uptime.size() ); i <= _max_stack; ++i )
      stack_uptime.push_back( new buff_uptime_t( sim ) );

  if ( initial_source ) // Player Buffs
  {
    cooldown = initial_source-> get_cooldown( "buff_" + name_str );
    if ( initial_source != player )
      name_str = name_str + ':' + initial_source -> name_str;
    rng = initial_source-> get_rng( name_str );
  }
  else // Sim Buffs
  {
    cooldown = sim -> get_cooldown( "buff_" + name_str );
    rng = sim -> get_rng( name_str );
  }
  cooldown -> duration = buff_cooldown;

}

// buff_t::~buff_t ==========================================================

buff_t::~buff_t()
{
  range::dispose( stack_uptime );
}

// buff_t::combat_begin ==========================================================

void buff_t::combat_begin()
{
  iteration_uptime_sum = timespan_t::zero();
}

// buff_t::combat_end ==========================================================

void buff_t::combat_end()
{
  if ( player )
    uptime_pct.add( player -> iteration_fight_length != timespan_t::zero() ? 100.0 * iteration_uptime_sum / player -> iteration_fight_length : 0 );
  else
    uptime_pct.add( sim -> current_time != timespan_t::zero() ? 100.0 * iteration_uptime_sum / sim -> current_time : 0 );

  for ( size_t i = 0; i < stack_uptime.size(); i++ )
    stack_uptime[ i ] -> combat_end();
}

// buff_t::may_react ========================================================

bool buff_t::may_react( int stack ) const
{
  if ( current_stack == 0    ) return false;
  if ( stack > current_stack ) return false;
  if ( stack < 1             ) return false;

  if ( stack > _max_stack ) return false;

  timespan_t occur = stack_occurrence[ stack ];

  if ( occur <= timespan_t::zero() ) return true;

  return sim -> current_time > stack_react_time[ stack ];
}

// buff_t::stack_react ======================================================

int buff_t::stack_react() const
{
  int stack = 0;

  for ( int i = 1; i <= current_stack; i++ )
  {
    if ( stack_react_time[ i ] > sim -> current_time ) break;
    stack++;
  }

  return stack;
}

// buff_t::remains ==========================================================

timespan_t buff_t::remains() const
{
  if ( current_stack <= 0 )
  {
    return timespan_t::zero();
  }
  if ( expiration )
  {
    return expiration -> occurs() - sim -> current_time;
  }
  return timespan_t::min();
}

// buff_t::remains_gt =======================================================

bool buff_t::remains_gt( timespan_t time ) const
{
  timespan_t time_remaining = remains();

  if ( time_remaining == timespan_t::zero() ) return false;

  if ( time_remaining == timespan_t::min() ) return true;

  return ( time_remaining > time );
}

// buff_t::remains_lt =======================================================

bool buff_t::remains_lt( timespan_t time ) const
{
  timespan_t time_remaining = remains();

  if ( time_remaining == timespan_t::min() ) return false;

  return ( time_remaining < time );
}

// buff_t::trigger ==========================================================

bool buff_t::trigger( action_t*  a,
                      int        stacks,
                      double     value,
                      timespan_t duration )
{
  double chance = default_chance;
  if ( chance < 0 ) chance = a -> ppm_proc_chance( -chance );
  return trigger( stacks, value, chance, duration );
}

// buff_t::trigger ==========================================================

bool buff_t::trigger( int        stacks,
                      double     value,
                      double     chance,
                      timespan_t duration )
{
  if ( _max_stack == 0 || chance == 0 ) return false;

  if ( cooldown -> remains() > timespan_t::zero() )
    return false;

  if ( player && player -> sleeping )
    return false;

  trigger_attempts++;

  if ( chance < 0 ) chance = default_chance;

  if ( ! rng -> roll( chance ) )
    return false;

  if ( ! activated && player && player -> in_combat && sim -> default_aura_delay > timespan_t::zero() )
  {
    // In-game, procs that happen "close to eachother" are usually delayed into the
    // same time slot. We roughly model this by allowing procs that happen during the
    // buff's already existing delay period to trigger at the same time as the first
    // delayed proc will happen.
    if ( delay )
    {
      buff_delay_t* d = dynamic_cast< buff_delay_t* >( delay );
      d -> stacks += stacks;
      d -> value = value;
    }
    else
      delay = new ( sim ) buff_delay_t( sim, player, this, stacks, value, duration );
  }
  else
    execute( stacks, value, duration );

  return true;
}

// buff_t::execute ==========================================================

void buff_t::execute( int stacks, double value, timespan_t duration )
{
  if ( last_trigger > timespan_t::zero() )
  {
    trigger_intervals.add( ( sim -> current_time - last_trigger ).total_seconds() );
  }
  last_trigger = sim -> current_time;

  if ( reverse )
  {
    decrement( stacks, value );
  }
  else
  {
    increment( stacks, value, duration );
  }

  // new buff cooldown impl
  if ( cooldown -> duration > timespan_t::zero() )
  {
    if ( sim -> debug )
      log_t::output( sim, "%s starts buff %s cooldown (%s) with duration %.2f",
                     ( source ? source -> name() : "someone" ), name_str.c_str(), cooldown -> name(), cooldown -> duration.total_seconds() );

    cooldown -> start();
  }

  trigger_successes++;
}

// buff_t::increment ========================================================

void buff_t::increment( int        stacks,
                        double     value,
                        timespan_t duration )
{
  if ( overridden ) return;

  if ( _max_stack == 0 ) return;

  if ( current_stack == 0 )
  {
    start( stacks, value, duration );
  }
  else
  {
    refresh( stacks, value, duration );
  }
}

// buff_t::decrement ========================================================

void buff_t::decrement( int    stacks,
                        double value )
{
  if ( overridden ) return;

  if ( _max_stack == 0 || current_stack <= 0 ) return;

  if ( stacks == 0 || current_stack <= stacks )
  {
    expire();
  }
  else
  {
    if ( static_cast<std::size_t>( current_stack ) < stack_uptime.size() )
      stack_uptime[ current_stack ] -> update( false );

    current_stack -= stacks;
    if ( value >= 0 ) current_value = value;

    if ( static_cast<std::size_t>( current_stack ) < stack_uptime.size() )
      stack_uptime[ current_stack ] -> update( true );

    if ( sim -> debug )
      log_t::output( sim, "buff %s decremented by %d to %d stacks",
                     name_str.c_str(), stacks, current_stack );
  }
}

// buff_t::extend_duration ==================================================

void buff_t::extend_duration( player_t* p, timespan_t extra_seconds )
{
  assert( expiration );
  assert( extra_seconds.total_seconds() < sim -> wheel_seconds );

  if ( extra_seconds > timespan_t::zero() )
  {
    expiration -> reschedule( expiration -> remains() + extra_seconds );

    if ( sim -> debug )
      log_t::output( sim, "%s extends buff %s by %.1f seconds. New expiration time: %.1f",
                     p -> name(), name_str.c_str(), extra_seconds.total_seconds(), expiration -> occurs().total_seconds() );
  }
  else if ( extra_seconds < timespan_t::zero() )
  {
    timespan_t reschedule_time = expiration -> remains() + extra_seconds;

    if ( reschedule_time <= timespan_t::zero() )
    {
      // When Strength of Soul removes the Weakened Soul debuff completely,
      // there's a delay before the server notifies the client. Modeling
      // this effect as a world lag.
      timespan_t lag, dev;

      lag = p -> world_lag_override ? p -> world_lag : sim -> world_lag;
      dev = p -> world_lag_stddev_override ? p -> world_lag_stddev : sim -> world_lag_stddev;
      reschedule_time = p -> rngs.lag_world -> gauss( lag, dev );
    }

    event_t::cancel( expiration );

    expiration = new ( sim ) expiration_t( sim, player, this, reschedule_time );

    if ( sim -> debug )
      log_t::output( sim, "%s decreases buff %s by %.1f seconds. New expiration time: %.1f",
                     p -> name(), name_str.c_str(), extra_seconds.total_seconds(), expiration -> occurs().total_seconds() );
  }
}

// buff_t::start ============================================================

void buff_t::start( int        stacks,
                    double     value,
                    timespan_t duration )
{
  if ( _max_stack == 0 ) return;

  if ( current_stack != 0 )
  {
    sim -> errorf( "buff_t::start assertion error current_stack is not zero, buff %s from %s.\n", name_str.c_str(), player -> name() );
    assert( 0 );
  }

  if ( sim -> current_time <= timespan_t::from_seconds( 0.01 ) ) constant = true;

  start_count++;

  bump( stacks, value );

  if ( last_start >= timespan_t::zero() )
  {
    start_intervals.add( ( sim -> current_time - last_start ).total_seconds() );
  }
  last_start = sim -> current_time;

  timespan_t d = ( duration >= timespan_t::zero() ) ? duration : buff_duration;
  if ( d > timespan_t::zero() )
  {
    expiration = new ( sim ) expiration_t( sim, player, this, d );
  }
}

// buff_t::refresh ==========================================================

void buff_t::refresh( int        stacks,
                      double     value,
                      timespan_t duration )
{
  if ( _max_stack == 0 ) return;

  refresh_count++;

  bump( stacks, value );

  timespan_t d = ( duration >= timespan_t::zero() ) ? duration : buff_duration;
  // Make sure we always cancel the expiration event if we get an
  // infinite duration
  if ( d == timespan_t::zero() )
    event_t::cancel( expiration );
  else
  {
    assert( d > timespan_t::zero() );
    // Infinite duration -> duration of d
    if ( unlikely( ! expiration ) )
      expiration = new ( sim ) expiration_t( sim, player, this, d );
    else
      expiration -> reschedule( d );
  }
}

// buff_t::bump =============================================================

void buff_t::bump( int stacks, double value )
{
  if ( _max_stack == 0 ) return;

  if ( value >= 0 ) current_value = value;

  if ( max_stack() < 0 )
  {
    current_stack += stacks;
  }
  else if ( current_stack < max_stack() )
  {
    int before_stack = current_stack;
    stack_uptime[ current_stack ] -> update( false );

    current_stack += stacks;
    if ( current_stack > max_stack() )
      current_stack = max_stack();

    stack_uptime[ current_stack ] -> update( true );

    aura_gain();

    timespan_t now = sim -> current_time;
    timespan_t react = now + ( player ? ( player -> total_reaction_time() ) : sim -> reaction_time );
    for ( int i = before_stack+1; i <= current_stack; i++ )
    {
      stack_occurrence[ i ] = now;
      stack_react_time[ i ] = react;
    }
  }
}

// buff_t::override =========================================================

void buff_t::override( int stacks, double value )
{
  if ( _max_stack == 0 ) return;
  if ( current_stack != 0 )
  {
    sim -> errorf( "buff_t::override assertion error current_stack is not zero, buff %s from %s.\n", name_str.c_str(), player -> name() );
    assert( 0 );
  }

  if ( value < 0.0 )
  {
    value = default_value;
  }

  buff_duration = timespan_t::zero();
  start( stacks, value );
  overridden = true;
}

// buff_t::expire ===========================================================

void buff_t::expire()
{
  if ( current_stack <= 0 ) return;
  event_t::cancel( expiration );
  source = 0;
  current_stack = 0;
  current_value = 0;
  aura_loss();
  if ( last_start >= timespan_t::zero() )
  {
    iteration_uptime_sum += sim -> current_time - last_start;
  }

  if ( sim -> target -> resources.base[ RESOURCE_HEALTH ] == 0 ||
       sim -> target -> resources.current[ RESOURCE_HEALTH ] > 0 )
    if ( ! overridden )
    {
      constant = false;
    }

  for ( size_t i = 0; i < stack_uptime.size(); i++ )
    stack_uptime[ i ] -> update( false );
}

// buff_t::predict ==========================================================

void buff_t::predict()
{
  // Guarantee that may_react() will return true if the buff is present.
  fill( &stack_occurrence[ 0 ], &stack_occurrence[ current_stack + 1 ], timespan_t::min() );
  fill( &stack_react_time[ 0 ], &stack_react_time[ current_stack + 1 ], timespan_t::min() );
}

// buff_t::aura_gain ========================================================

void buff_t::aura_gain()
{
  if ( sim -> log )
  {
    std::string s = name_str + "_" + util_t::to_string( current_stack );

    if ( player )
    {
      if ( sim -> log && ! player->sleeping )
      {
        log_t::output( sim, "%s gains %s ( value=%.2f )", player->name(), s.c_str(), current_value );
      }
    }
    else
    {
      if ( sim -> log ) log_t::output( sim, "Raid gains %s", s.c_str() );
    }
  }
}

// buff_t::aura_loss ========================================================

void buff_t::aura_loss()
{
  if ( player )
  {
    if ( sim -> log && ! player -> sleeping )
      log_t::output( sim, "%s loses %s", player -> name(), name_str.c_str() );
  }
  else
  {
    if ( sim -> log ) log_t::output( sim, "Raid loses %s",  name_str.c_str() );
  }
}

// buff_t::reset ============================================================

void buff_t::reset()
{
  event_t::cancel( delay );
  cooldown -> reset();
  expire();
  last_start = timespan_t::min();
  last_trigger = timespan_t::min();
}

// buff_t::merge ============================================================

void buff_t::merge( const buff_t* other )
{
  start_intervals.merge( other -> start_intervals );
  trigger_intervals.merge( other -> trigger_intervals );
  up_count              += other -> up_count;
  down_count            += other -> down_count;
  start_count           += other -> start_count;
  refresh_count         += other -> refresh_count;
  trigger_attempts      += other -> trigger_attempts;
  trigger_successes     += other -> trigger_successes;

  uptime_pct.merge( other -> uptime_pct );

  if ( stack_uptime.size() != other -> stack_uptime.size() )
  {
    sim->errorf( "buff_t::merge buff %s of player %s stack_uptime vector not of equal length.\n", name_str.c_str(), player ? player -> name() : "" );
    assert( 0 );
  }
  for ( size_t i = 0; i < stack_uptime.size(); i++ )
    stack_uptime[ i ] -> merge ( *( other -> stack_uptime[ i ] ) );
}

// buff_t::analyze ==========================================================

void buff_t::analyze()
{
  if ( up_count > 0 )
  {
    benefit_pct = 100.0 * up_count / ( up_count + down_count );
  }
  if ( trigger_attempts > 0 )
  {
    trigger_pct = 100.0 * trigger_successes / trigger_attempts;
  }
  start_intervals.analyze();
  trigger_intervals.analyze();
  avg_start   =   start_count / ( double ) sim -> iterations;
  avg_refresh = refresh_count / ( double ) sim -> iterations;
  uptime_pct.analyze();

  for ( size_t i = 0; i < stack_uptime.size(); i++ )
    stack_uptime[ i ] -> analyze();
}

// buff_t::find =============================================================

buff_t* buff_t::find( const std::vector<buff_t*>& b, const std::string& name_str )
{
  for ( size_t i = 0; i < b.size(); i++ )
  {
    if ( name_str == b[ i ] -> name_str )
      return b[ i ];
  }

  return NULL;
}

// buff_t::to_str ===========================================================

std::string buff_t::to_str() const
{
  std::ostringstream s;

  s << data().to_str();
  s << " max_stack=" << _max_stack;
  s << " initial_stack=" << data().initial_stacks();
  s << " cooldown=" << cooldown -> duration.total_seconds();
  s << " duration=" << buff_duration.total_seconds();
  s << " default_chance=" << default_chance;

  return s.str();
}

// buff_t::create_expression ================================================

expr_t* buff_t::create_expression( const std::string& type )
{
  class buff_expr_t : public expr_t
  {
  public:
    buff_t& buff;
    buff_expr_t( const std::string& name, buff_t* b ) :
      expr_t( name ), buff( *b ) { assert( b ); }
  };

  if ( type == "remains" )
    return make_mem_fn_expr( name_str, *this, &buff_t::remains );

  else if ( type == "cooldown_remains" )
    return make_mem_fn_expr( name_str, *cooldown, &cooldown_t::remains );

  else if ( type == "up" )
  {
    struct buff_up_expr_t : public buff_expr_t
    {
      buff_up_expr_t( buff_t* b ) : buff_expr_t( "buff_up", b ) {}
      virtual double evaluate() { return buff.check() > 0; }
    };
    return new buff_up_expr_t( this );
  }

  else if ( type == "down" )
  {
    struct buff_down_expr_t : public buff_expr_t
    {
      buff_down_expr_t( buff_t* b ) : buff_expr_t( "buff_down", b ) {}
      virtual double evaluate() { return buff.check() <= 0; }
    };
    return new buff_down_expr_t( this );
  }

  else if ( type == "stack" )
    return make_mem_fn_expr( type, *this, &buff_t::check );

  else if ( type == "value" )
    return make_mem_fn_expr( type, *this, &buff_t::value );

  else if ( type == "react" )
    return make_mem_fn_expr( type, *this, &buff_t::stack_react );

  else if ( type == "cooldown_react" )
  {
    struct buff_cooldown_react_expr_t : public buff_expr_t
    {
      buff_cooldown_react_expr_t( buff_t* b ) :
        buff_expr_t( "buff_cooldown_react", b ) {}
      virtual double evaluate()
      {
        if ( buff.check() && ! buff.may_react() )
          return 0;
        else
          return buff.cooldown -> remains().total_seconds();
      }
    };
    return new buff_cooldown_react_expr_t( this );
  }

  return 0;
}

// ==========================================================================
// STAT_BUFF
// ==========================================================================

// stat_buff_t::stat_buff_t =================================================

stat_buff_t::stat_buff_t( const stat_buff_creator_t& params ) :
  buff_t( params.bc ), amount( params._amount ), stat( params._stat )
{
}

// stat_buff_t::bump ========================================================

void stat_buff_t::bump( int stacks, double value )
{
  if ( value > 0 )
  {
    amount = value;
  }
  buff_t::bump( stacks );
  double delta = amount * current_stack - current_value;
  if ( delta > 0 )
  {
    player -> stat_gain( stat, delta, 0, 0, buff_duration > timespan_t::zero() );
    current_value += delta;
  }
  else
    assert( delta == 0 );
}

// stat_buff_t::decrement ===================================================

void stat_buff_t::decrement( int stacks, double /* value */ )
{
  if ( stacks == 0 || current_stack <= stacks )
  {
    expire();
  }
  else
  {
    double delta = amount * stacks;
    player -> stat_loss( stat, delta, 0, 0, buff_duration > timespan_t::zero() );
    current_stack -= stacks;
    current_value -= delta;
  }
}

// stat_buff_t::expire ======================================================

void stat_buff_t::expire()
{
  if ( current_stack > 0 )
  {
    player -> stat_loss( stat, current_value, 0, 0, buff_duration > timespan_t::zero() );
    buff_t::expire();
  }
}

// ==========================================================================
// COST_REDUCTION_BUFF
// ==========================================================================

cost_reduction_buff_t::cost_reduction_buff_t( const cost_reduction_buff_creator_t& params ) :
  buff_t( params.bc ), amount( params._amount ), school( params._school ), refreshes( params._refreshes )
{
}

// cost_reduction_buff_t::bump ==============================================

void cost_reduction_buff_t::bump( int stacks,double value )
{
  if ( value > 0 )
  {
    amount = value;
  }
  buff_t::bump( stacks );
  double delta = amount * current_stack - current_value;
  if ( delta > 0 )
  {
    player -> cost_reduction_gain( school, delta );
    current_value += delta;
  }
  else assert( delta == 0 );
}

// cost_reduction_buff_t::decrement =========================================

void cost_reduction_buff_t::decrement( int stacks, double /* value */ )
{
  if ( stacks == 0 || current_stack <= stacks )
  {
    expire();
  }
  else
  {
    double delta = amount * stacks;
    player -> cost_reduction_loss( school, delta );
    current_stack -= stacks;
    current_value -= delta;
  }
}

// cost_reduction_buff_t::expire ============================================

void cost_reduction_buff_t::expire()
{
  if ( current_stack > 0 )
  {
    player -> cost_reduction_loss( school, current_value );
    buff_t::expire();
  }
}

// cost_reduction_buff_t::refresh ===========================================

void cost_reduction_buff_t::refresh( int        stacks,
                                     double     value,
                                     timespan_t duration )
{
  if ( ! refreshes )
  {
     refresh_count++;

    bump( stacks, value );
    return;
  }

  buff_t::refresh( stacks, value, duration );
}

// ==========================================================================
// DEBUFF
// ==========================================================================

debuff_t::debuff_t( const buff_creator_t& params ) :
  buff_t( params )
{}

void buff_creator_t::init()
{
  _chance = -1.0;
  _max_stack = -1;
  _duration = timespan_t::min();
  _cooldown = timespan_t::min();
  _quiet = -1;
  _reverse = -1;
  _activated = -1;
  _default_value = -1.0;
}

buff_creator_t::buff_creator_t( actor_pair_t p, const std::string& n, const spell_data_t* sp ) :
  _player( p ), _sim( p.source->sim ), _name( n ), s_data( sp )
{ init(); }

buff_creator_t::buff_creator_t( actor_pair_t p , uint32_t id, const std::string& n ) :
  _player( p ), _sim( p.source->sim ), _name( n ), s_data( _player.source ? _player.source->find_spell( id ) : spell_data_t::nil() )
{ init(); }

buff_creator_t::buff_creator_t( sim_t* s, const std::string& n, const spell_data_t* sp ) :
  _player( actor_pair_t() ), _sim( s ), _name( n ), s_data( sp )
{ init(); }
