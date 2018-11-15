#ifndef __INCLUDE_KAFARR_HPP__
#define __INCLUDE_KAFARR_HPP__

#include <memory>
#include <algorithm>
#include <chrono>

#include <librdkafka/rdkafkacpp.h>
#include <libserdes/serdescpp.h>
#include <libserdes/serdescpp-avro.h>


#include "arrow/array.h"
#include "arrow/record_batch.h"
#include "arrow/status.h"
#include "arrow/table.h"
#include "arrow/table_builder.h"
#include "arrow/array.h"
#include "arrow/buffer.h"
#include "arrow/builder.h"
#include "arrow/memory_pool.h"


#include "err.hpp"
#include "kfk_hlpr.hpp"
#include "avr_hlpr.hpp"

namespace kafarr {
  /**
   *
   */  
  class bse{
  protected:
    std::unique_ptr<Serdes::Avro> _srds;
    
  public:
    bse() = delete;
    bse(const std::string& reg_url):_srds(kfk_hlpr::mk_srds(reg_url))
    {
      std::cerr << "bse::bse(..)\n";
    }
    
    virtual ~bse(){
      std::cerr << "bse::~bse()\n";
    }
  };

  /**
   *
   */
  class lstnr : bse {
  private :
    const int RD_KFK_POLL_MS = 5;
    const std::unique_ptr<RdKafka::KafkaConsumer> _cnsmr;
    
  public:
    lstnr() = delete;
    lstnr(const std::string& srvr_lst,
	  const std::string& grp,
	  const std::vector<std::string>& tpcs,
	  const std::string& reg_url) : bse(reg_url),_cnsmr(kfk_hlpr::mk_kfk_cnsmr(grp, srvr_lst))
    {      
      auto err = _cnsmr->subscribe(tpcs);       
      
      if (err != RdKafka::ErrorCode::ERR_NO_ERROR){
	std::string msg = "failed to subscribe to topics: ";
	std::for_each(tpcs.begin(), tpcs.end(), [&](auto tpc){msg.append(tpc).append(", ");});	
      	msg.append(RdKafka::err2str(err));
      	throw kafarr::err(msg);
      }
    }
    
  public:
    /**
     * destructor
     */
    ~lstnr(){
      _cnsmr->close();
    }

  private:
    auto now_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    
  public:
    void poll(int n_msgs, std::shared_ptr<arrow::RecordBatch>* out, int max_tme_ms = 1000) {
      // no known schema id yet...
      int _val_blck_sch_id = -1;

      Serdes::Schema* schema = NULL;
      avro::GenericDatum *dtm = NULL;
      std::string err;
      std::unique_ptr<arrow::RecordBatchBuilder> bldr;
            
      auto [n, go]=std::tuple(0, true);
      auto [t0, t00] = std::tuple(now_ms(), now_ms());
      
      while(go) {
	// poll kafka
	const std::shared_ptr<RdKafka::Message> msg(_cnsmr->consume(max_tme_ms));
	
	// check if result is an actual messgage
	if(kfk_hlpr::is_msg(msg)){
	  // we have a message
	  // make sure there is no key, we currentlly don't process keys
	  if(msg->key()) throw kafarr::err("keyed messages currentlly not handled! ");

	  // if there is a payload
	  if(msg->payload()){
	    int msg_val_sch_id = -1;
	    
	    if(kfk_hlpr::val_cp1(msg)){
	      // so message value is schema encoded - get schema id
	      const int msg_val_sch_id = kfk_hlpr::schm_id(msg->payload());
	      
	      if(_val_blck_sch_id == -1) {
		// first message in a block. set schema
		t00 = now_ms();  //DBG
		std::cerr << "DEBUG:>> time waiting for the first message: " <<  (t00 - t0) << "ms" <<  std::endl;
		_val_blck_sch_id = msg_val_sch_id;

		if(_srds->deserialize(&schema, &dtm, msg->payload(), msg->len(), err) == -1)
		  throw kafarr::err(" failed to deserialise messge: ", err);

		auto arr_schm = avr_hlpr::mk_arrw_schm(schema);
		auto pool = arrow::default_memory_pool();
		arrow::RecordBatchBuilder::Make(arr_schm, pool, &bldr);
		avr_hlpr::rd_dta(msg->offset(), dtm, bldr);
		delete dtm;
	      }
	      else if(_val_blck_sch_id == msg_val_sch_id) {
		if(_srds->deserialize(&schema, &dtm, msg->payload(), msg->len(), err) == -1)
		  throw kafarr::err(" failed to deserialise messge: ", err);
		avr_hlpr::rd_dta(msg->offset(), dtm, bldr);
		delete dtm;
	      }
	      else {
		// TODO:>> this case needs testing
		//std::cerr<< "BREAK in schema. Previous was " << _val_blck_sch_id << ", current is : " << msg_val_sch_id << std::endl;		
		std::cerr << "rejecting message @ offset " << msg->offset() << " on topic::" << msg->topic_name() << " [" << msg->partition() << "]" << std::endl;	
		//read the state of client on partition message arrived on
		std::vector<RdKafka::TopicPartition*> prtns{RdKafka::TopicPartition::create(msg->topic_name(), msg->partition())};
		_cnsmr->position(prtns);
		std::cerr << "rejecting message @ offset " << prtns[0]->offset() << " on topic::" << prtns[0]->topic() << " [" << prtns[0]->partition() << "]" << std::endl;
		
		//set a modified offset
		prtns[0]->set_offset(msg->offset());
		std::cerr << "Resseting offset to:: [" << prtns[0]->offset() <<  "]@" << prtns[0]->partition() << std::endl;
		//
		
		//_cnsmr->offsets_store(prtns);
		_cnsmr->seek(*prtns[0], 1000);
		_cnsmr->commitSync(prtns);
		
		go = false;
		
		// TODO:>> remove once the above code is tested
		throw kafarr::err("BUG. Break in schema is not handled.");
	      }
	    }
	    else
	      throw kafarr::err("only schema encoded messages currentlly handled");
	  }
	  // if message max or timed out
	  if (++n >= n_msgs) go = false;
        }
	if (now_ms() - t0 > max_tme_ms) go = false;
      }

      if(schema) delete(schema);

      {
	auto tpop = now_ms() - t00;
	std::cerr << "DEBUG:>> time popping messages: " <<  (now_ms() - t00) << "ms" <<   std::endl;
      }
      _cnsmr->commitSync();
      
      if(n > 0) bldr->Flush(out);
    }

    // TODO:>>  
    void ex_tst(const std::string& msg) {      
      std::cerr << "about to throw TEST exception: " << msg << "\n";
      throw kafarr::err("thrown TEST exception. msg: " + msg);
    }
    
  private:
    /**
     * use serdes to decode the message
     std::tuple<std::shared_ptr<Serdes::Schema>, std::shared_ptr<avro::GenericDatum> > get_msg_schma(const void *buff, size_t len) {
     avro::GenericDatum *d = NULL;
     Serdes::Schema* schema = NULL;
     std::string _err;
     if(_srds->deserialize(&schema, &d, buff, len, _err) == -1)
     throw kafarr::err(" failed to deserialise messge: ", _err);
     return {std::unique_ptr<Serdes::Schema>(schema), std::shared_ptr<avro::GenericDatum>(d)};
     }
    */
    
  };
  
}

#endif //  __INCLUDE_KAFARR_HPP__