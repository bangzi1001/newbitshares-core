/*
 * Copyright (c) 2020 Abit More, and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <graphene/chain/ticket_object.hpp>

#include <graphene/chain/database.hpp>
#include <graphene/chain/exceptions.hpp>
#include <graphene/chain/hardfork.hpp>

#include <fc/io/raw.hpp>
#include <iostream>
#include <ctime>

using namespace graphene::chain;

void ticket_object::init_new( time_point_sec now, account_id_type new_account,
                              ticket_type new_target_type, const asset& new_amount )
{
   account = new_account;
   target_type = new_target_type;
   amount = new_amount;

   current_type = liquid;
   status = charging;
   next_auto_update_time = now + seconds_per_charging_step;
   next_type_downgrade_time = time_point_sec::maximum();

   update_value();
}
//新起锁仓
void ticket_object::init_split( time_point_sec now, const ticket_object& old_ticket,
                                ticket_type new_target_type, const asset& new_amount )
{
   account = old_ticket.account;
   target_type = old_ticket.target_type;
   amount = new_amount;

   current_type = old_ticket.current_type;
   status = old_ticket.status;
   next_auto_update_time = time_point_sec::maximum();;
   next_type_downgrade_time = old_ticket.next_type_downgrade_time;

   update_target_type( now, new_target_type );
}
//永远无法升级到下一阶段
void ticket_object::update_target_type( time_point_sec now, ticket_type new_target_type )
{
   //const database& _db = db();
   //const auto block_time = _db.head_block_time();

   if( current_type < new_target_type )
   {
      if( status != charging )
      {
         status = charging;
         next_auto_update_time = time_point_sec::maximum();;
      }
      // else do nothing here
   }
   else // current_type >= new_target_type
   {
      status = withdrawing;
      if( next_type_downgrade_time == time_point_sec::maximum() ) // no downgrade was started ago
      {
         if( current_type == new_target_type ) // was charging, to cancel
            next_type_downgrade_time = now + seconds_to_cancel_charging;
         else // was stable or charging, to downgrade
         {
            current_type = static_cast<ticket_type>( static_cast<uint8_t>(current_type) - 1 );
            if( HARDFORK_CORE_2103F_PASSED(now) )
               next_type_downgrade_time = now + seconds_to_downgrade(current_type);
            else
               next_type_downgrade_time = now + seconds_to_downgrade(current_type);

         }
      }
      // else a downgrade was started ago, keep the old value of `next_type_downgrade_time`
      // Note: it's possible that `next_type_downgrade_time` is in the past,
      //       in this case, it will be processed in database::process_tickets().
      next_auto_update_time = next_type_downgrade_time;
   }
   target_type = new_target_type;

   update_value();
}

void ticket_object::adjust_amount( const asset& delta_amount )
{
   amount += delta_amount;
   update_value();
}

void ticket_object::auto_update()
{
   //const database& _db = db();
   //const auto block_time = _db.head_block_time();
   time_point_sec now = time_point_sec( time_point::now() ) ;  

   if( status == charging )
   {
      current_type = static_cast<ticket_type>( static_cast<uint8_t>(current_type) + 1 );
      next_type_downgrade_time = time_point_sec::maximum();
      if( current_type < target_type )
      {
         next_auto_update_time = time_point_sec::maximum();
      }
      else // reached target, stop
      {
         if( current_type != lock_forever )
         {
            status = stable;
            next_auto_update_time = time_point_sec::maximum();
         }
         else // lock forever
         {
            status = withdrawing;
            next_auto_update_time += seconds_per_lock_forever_update_step;
            if( HARDFORK_CORE_2103F_PASSED(now) )
            {
               value = amount.amount * 1;
            }
            else
            {
               value = amount.amount * value_multiplier(current_type);
            }
         }
      }
   }
   else // status == withdrawing
   {
      // Note: current_type != liquid, guaranteed by the caller
      if( current_type == lock_forever )
      {
         share_type delta_value = 0;
         if( HARDFORK_CORE_2103F_PASSED(now) )
         {
            delta_value = amount.amount * 1 / lock_forever_update_steps;
         }
         else
         {         
            delta_value = amount.amount * value_multiplier(current_type) / lock_forever_update_steps;
         }
         
         if( delta_value <= 0 )
            delta_value = 1;
         if( value <= delta_value )
         {
            value = 0;
            status = stable;
            next_auto_update_time = time_point_sec::maximum();
         }
         else
         {
            value -= delta_value;
            next_auto_update_time += seconds_per_lock_forever_update_step;
         }
      }
      else if( current_type == target_type ) // finished downgrading
      {
         status = stable;
         next_type_downgrade_time = time_point_sec::maximum();
         next_auto_update_time = time_point_sec::maximum();
      }
      else // to downgrade
      {
         current_type = static_cast<ticket_type>( static_cast<uint8_t>(current_type) - 1 );
         if( HARDFORK_CORE_2103F_PASSED(now) )
         {
            next_type_downgrade_time = next_type_downgrade_time + ( seconds_to_downgrade(current_type) * 10 );
         }
         else
         {
            next_type_downgrade_time += seconds_to_downgrade(current_type);
         }
         next_auto_update_time = time_point_sec::maximum();;
      }
   }

   update_value();
}

void ticket_object::update_value()
{
   //const database& _db = db();
   //const auto block_time = _db.head_block_time();
   time_point_sec now = time_point_sec( time_point::now() ) ;

   if( current_type != lock_forever )
   {
      if( HARDFORK_CORE_2103F_PASSED(now) )
         value = amount.amount * 1;
      else
         value = amount.amount * value_multiplier(current_type);
   }
   // else lock forever and to be updated, do nothing here
}

FC_REFLECT_DERIVED_NO_TYPENAME( graphene::chain::ticket_object, (graphene::db::object),
                    (account)
                    (target_type)
                    (amount)
                    (current_type)
                    (status)
                    (value)
                    (next_auto_update_time)
                    (next_type_downgrade_time)
                  )

GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::chain::ticket_object )
