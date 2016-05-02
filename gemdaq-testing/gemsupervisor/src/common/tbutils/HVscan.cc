#include "gem/supervisor/tbutils/HVscan.h"

#include "gem/hw/vfat/HwVFAT2.h"
#include "gem/hw/glib/HwGLIB.h"
#include "gem/hw/optohybrid/HwOptoHybrid.h"

#include "gem/utils/GEMLogging.h"

#include "TH1.h"
#include "TFile.h"
#include "TCanvas.h"
#include "TROOT.h"
#include "TString.h"
#include "TError.h"
 
#include <algorithm>
#include <iomanip>
#include <ctime>
#include <queue>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>

#include "cgicc/HTTPRedirectHeader.h"
#include "gem/supervisor/tbutils/VFAT2XMLParser.h"
#include "TStopwatch.h"

#include <iostream>
#include "xdata/Vector.h"
#include <string>

#include "xoap/MessageReference.h"
#include "xoap/MessageFactory.h"
#include "xoap/SOAPEnvelope.h"
#include "xoap/SOAPConstants.h"
#include "xoap/SOAPBody.h"
#include "xoap/Method.h"
#include "xoap/AttachmentPart.h"

#include "xoap/domutils.h"

#include "gem/hw/glib/GLIBManager.h"

XDAQ_INSTANTIATOR_IMPL(gem::supervisor::tbutils::HVscan)

void gem::supervisor::tbutils::HVscan::ConfigParams::registerFields(xdata::Bag<ConfigParams> *bag)
{
  minHV     =    0;
  maxHV     =  100;
  stepSize  =  10U;

  bag->addField("minHV", &minHV);
  bag->addField("maxHV", &maxHV);
  bag->addField("stepSize",  &stepSize );

}


gem::supervisor::tbutils::HVscan::HVscan(xdaq::ApplicationStub * s) throw (xdaq::exception::Exception) :
  gem::supervisor::tbutils::GEMTBUtil(s)
{

  getApplicationInfoSpace()->fireItemAvailable("scanParams", &scanParams_);
  getApplicationInfoSpace()->fireItemValueRetrieve("scanParams", &scanParams_);

  // HyperDAQ bindings
  xgi::framework::deferredbind(this, this, &gem::supervisor::tbutils::HVscan::webDefault,      "Default"    );
  xgi::framework::deferredbind(this, this, &gem::supervisor::tbutils::HVscan::webConfigure,    "Configure"  );
  xgi::framework::deferredbind(this, this, &gem::supervisor::tbutils::HVscan::webStart,        "Start"      );
  runSig_   = toolbox::task::bind(this, &HVscan::run,        "run"       );

  // Initiate and activate main workloop  
  wl_ = toolbox::task::getWorkLoopFactory()->getWorkLoop("urn:xdaq-workloop:GEMTestBeamSupervisor:HVscan","waiting");
  wl_->activate();

}

gem::supervisor::tbutils::HVscan::~HVscan()
{
  wl_ = toolbox::task::getWorkLoopFactory()->getWorkLoop("urn:xdaq-workloop:GEMTestBeamSupervisor:HVscan","waiting");
  //should we check to see if it's running and try to stop?
  wl_->cancel();
  wl_ = 0;

}

// State transitions
bool gem::supervisor::tbutils::HVscan::run(toolbox::task::WorkLoop* wl)
{
  wl_semaphore_.take(); //teake workloop
  if (!is_running_) {
    wl_semaphore_.give(); // give work loop if it is not running
    uint32_t bufferDepth = 0;
    bufferDepth = glibDevice_->getFIFOVFATBlockOccupancy(readout_mask);
    LOG4CPLUS_INFO(getApplicationLogger()," ******IT IS NOT RUNNIG ***** ");
    return false;
  }

  //send triggers
  hw_semaphore_.take(); //take hw to send the trigger 

  sendAMC13trigger();

  optohybridDevice_->setTrigSource(0x0);// trigger sources   
  confParams_.bag.triggersSeen = optohybridDevice_->getL1ACount(0x0);

  LOG4CPLUS_INFO(getApplicationLogger(), " ABC TriggersSeen " << confParams_.bag.triggersSeen);

  hw_semaphore_.give(); //give hw to send the trigger 
  
  // if triggersSeen < N triggers
  if ((uint64_t)(confParams_.bag.triggersSeen) < (uint64_t)(confParams_.bag.nTriggers)) {
    
    hw_semaphore_.take(); // take hw to set buffer depth

    // Get the size of GLIB data buffer       
    uint32_t bufferDepth = 0;
    bufferDepth  = glibDevice_->getFIFOOccupancy(readout_mask); 

    LOG4CPLUS_INFO(getApplicationLogger(), " Bufferdepht " << bufferDepth);    
      
    hw_semaphore_.give(); // give hw to set buffer depth
    wl_semaphore_.give();//give workloop to read
    return true;
  }//end if triggerSeen < nTrigger
  else {
    
    hw_semaphore_.take(); //take hw to set Runmode 0 on VFATs 
    for (auto chip = vfatDevice_.begin(); chip != vfatDevice_.end(); ++chip) {
      (*chip)->setRunMode(0);
    }// end for  





    
    uint32_t bufferDepth = 0;
    bufferDepth = glibDevice_->getFIFOVFATBlockOccupancy(readout_mask);
        
    //reset counters
    optohybridDevice_->resetL1ACount(0x5);
    optohybridDevice_->resetResyncCount();
    optohybridDevice_->resetBC0Count();
    optohybridDevice_->resetCalPulseCount(0x1);
    
    hw_semaphore_.give();  // give hw to reset counters

    LOG4CPLUS_INFO(getApplicationLogger()," ABC Scan point TriggersSeen " 
		   << confParams_.bag.triggersSeen );

    //paused
    sleep(0.001);
    
    hw_semaphore_.take(); // take hw to stop workloop
    wl_->submit(stopSig_);  
    hw_semaphore_.give(); // give hw to stop workloop
    wl_semaphore_.give(); // end of workloop	      
    return true; 
  }//end else triggerseen < N triggers   
  return false; 
}//end run

void gem::supervisor::tbutils::HVscan::scanParameters(xgi::Output *out)
  throw (xgi::exception::Exception)
{
  try {
    std::string isReadonly = "";
    if (is_running_ || is_configured_)
      isReadonly = "readonly";
    *out << cgicc::span()   << std::endl

	 << cgicc::label("MinHV").set("for","MinHV") << std::endl
	 << cgicc::input().set("id","MinHV").set(is_running_?"readonly":"").set("name","MinHV")
      .set("type","number").set("min","-255").set("max","255")
      .set("value",boost::str(boost::format("%d")%(scanParams_.bag.minHV)))
	 << std::endl

	 << cgicc::label("MaxHV").set("for","MaxHV") << std::endl
	 << cgicc::input().set("id","MaxHV").set(is_running_?"readonly":"").set("name","MaxHV")
      .set("type","number").set("min","-255").set("max","255")
      .set("value",boost::str(boost::format("%d")%(scanParams_.bag.maxHV)))
	 << std::endl
	 << cgicc::br() << std::endl

	 << cgicc::label("VStep").set("for","VStep") << std::endl
	 << cgicc::input().set("id","VStep").set(is_running_?"readonly":"").set("name","VStep")
      .set("type","number").set("min","1").set("max","255")
      .set("value",boost::str(boost::format("%d")%(scanParams_.bag.stepSize)))
	 << std::endl
	 << cgicc::br() << std::endl

	 << cgicc::label("NTrigsStep").set("for","NTrigsStep") << std::endl
	 << cgicc::input().set("id","NTrigsStep").set(is_running_?"readonly":"").set("name","NTrigsStep")
      .set("type","number").set("min","0")
      .set("value",boost::str(boost::format("%d")%(confParams_.bag.nTriggers)))
	 << cgicc::br() << std::endl
	 << cgicc::label("NTrigsSeen").set("for","NTrigsSeen") << std::endl
	 << cgicc::input().set("id","NTrigsSeen").set("name","NTrigsSeen")
      .set("type","number").set("min","0").set("readonly")
      .set("value",boost::str(boost::format("%d")%(confParams_.bag.triggersSeen)))
	 << cgicc::br() << std::endl
	 << cgicc::span()   << std::endl;
  }
  catch (const xgi::exception::Exception& e) {
    LOG4CPLUS_INFO(this->getApplicationLogger(),"Something went wrong displaying VFATS(xgi): " << e.what());
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  catch (const std::exception& e) {
    LOG4CPLUS_INFO(this->getApplicationLogger(),"Something went wrong displaying VFATS(std): " << e.what());
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
}

// HyperDAQ interface
void gem::supervisor::tbutils::HVscan::webDefault(xgi::Input *in, xgi::Output *out)
  throw (xgi::exception::Exception)
{
  //LOG4CPLUS_INFO(this->getApplicationLogger(),"gem::supervisor::tbutils::HVscan::webDefaul");
  try {
    ////update the page refresh 
    if (!is_working_ && !is_running_) {
    }
    else if (is_working_) {
      cgicc::HTTPResponseHeader &head = out->getHTTPResponseHeader();
      head.addHeader("Refresh","2");
    }
    else if (is_running_) {
      cgicc::HTTPResponseHeader &head = out->getHTTPResponseHeader();
      head.addHeader("Refresh","5");
    }
    
    //generate the control buttons and display the ones that can be touched depending on the run mode
    *out << "<div class=\"xdaq-tab-wrapper\">"            << std::endl;
    *out << "<div class=\"xdaq-tab\" title=\"Control\">"  << std::endl;

    *out << "<table class=\"xdaq-table\">" << std::endl
	 << cgicc::thead() << std::endl
	 << cgicc::tr()    << std::endl //open
	 << cgicc::th()    << "Control" << cgicc::th() << std::endl
	 << cgicc::th()    << "Buffer"  << cgicc::th() << std::endl
	 << cgicc::tr()    << std::endl //close
	 << cgicc::thead() << std::endl 
      
	 << "<tbody>" << std::endl
	 << "<tr>"    << std::endl
	 << "<td>"    << std::endl;
    
    if (!is_initialized_) {
      //have a menu for selecting the VFAT
      *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Initialize") << std::endl;

      selectCHAMBER(out);
      scanParameters(out);
      
      *out << cgicc::input().set("type", "submit")
	.set("name", "command").set("title", "Initialize hardware acces.")
	.set("value", "Initialize") << std::endl;

      *out << cgicc::form() << std::endl;
    }
    
    else if (!is_configured_) {
      //this will allow the parameters to be set to the chip and scan routine

      *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Configure") << std::endl;

      selectCHAMBER(out);

      scanParameters(out);

      *out << cgicc::input().set("type","text").set("name","xmlFilename").set("size","80")
	.set("ENCTYPE","multipart/form-data").set("readonly")
	.set("value",confParams_.bag.settingsFile.toString()) << std::endl;
      
      *out << cgicc::br() << std::endl;
      *out << cgicc::input().set("type", "submit")
	.set("name", "command").set("title", "Configure threshold scan.")
	.set("value", "Configure") << std::endl;
      *out << cgicc::form()        << std::endl;
    }
    
    else if (!is_running_) {

      //hardware is initialized and configured, we can start the run
      *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Start") << std::endl;

      selectCHAMBER(out);

      scanParameters(out);
      
      *out << cgicc::input().set("type", "submit")
	.set("name", "command").set("title", "Start threshold scan.")
	.set("value", "Start") << std::endl;
      *out << cgicc::form()    << std::endl;
    }
    
    else if (is_running_) {
      *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Stop") << std::endl;
      
      selectCHAMBER(out);
      scanParameters(out);
      
      *out << cgicc::input().set("type", "submit")
	.set("name", "command").set("title", "Stop threshold scan.")
	.set("value", "Stop") << std::endl;
      *out << cgicc::form()   << std::endl;
    }
    
    *out << cgicc::comment() << "end the main commands, now putting the halt/reset commands" << cgicc::comment() << cgicc::br() << std::endl;
    *out << cgicc::span()  << std::endl
	 << "<table>" << std::endl
	 << "<tr>"    << std::endl
	 << "<td>"    << std::endl;
      
    //always should have a halt command
    *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Halt") << std::endl;
    
    *out << cgicc::input().set("type", "submit")
      .set("name", "command").set("title", "Halt threshold scan.")
      .set("value", "Halt") << std::endl;
    *out << cgicc::form() << std::endl
	 << "</td>" << std::endl;
    
    *out << "<td>"  << std::endl;

    if (!is_running_) {
      //comand that will take the system to initial and allow to change the hw device
      *out << cgicc::form().set("method","POST").set("action", "/" + getApplicationDescriptor()->getURN() + "/Reset") << std::endl;
      *out << cgicc::input().set("type", "submit")
	.set("name", "command").set("title", "Reset device.")
	.set("value", "Reset") << std::endl;
      *out << cgicc::form() << std::endl;
    }
    *out << "</td>"    << std::endl
	 << "</tr>"    << std::endl
	 << "</table>" << std::endl
	 << cgicc::br() << std::endl
	 << cgicc::span()  << std::endl;

    *out << "</td>" << std::endl;

    *out << "<td>" << std::endl;
    if (is_initialized_)
      showBufferLayout(out);
    *out << "</td>"    << std::endl
	 << "</tr>"    << std::endl
	 << "</tbody>" << std::endl
	 << "</table>" << cgicc::br() << std::endl;
    
    *out << "</div>" << std::endl;
    
    *out << "<div class=\"xdaq-tab\" title=\"Counters\">"  << std::endl;
    if (is_initialized_)
      showCounterLayout(out);
    *out << "</div>" << std::endl;

    *out << "<div class=\"xdaq-tab\" title=\"Fast Commands/Trigger Setup\">"  << std::endl;
//open fast commands
    if (is_initialized_)
      fastCommandLayout(out);
    *out << "</div>" << std::endl; //close fastcommands
    
    *out << "</div>" << std::endl;

    *out << cgicc::br() << cgicc::br() << std::endl;
    
    *out << "<table class=\"xdaq-table\">" << std::endl
	 << cgicc::thead() << std::endl
	 << cgicc::tr()    << std::endl //open
	 << cgicc::th()    << "Program" << cgicc::th() << std::endl
	 << cgicc::th()    << "System"  << cgicc::th() << std::endl
	 << cgicc::tr()    << std::endl //close
	 << cgicc::thead() << std::endl 
      
	 << "<tbody>" << std::endl
	 << "<tr>"    << std::endl
	 << "<td>"    << std::endl;

    *out << "<table class=\"xdaq-table\">" << std::endl
	 << cgicc::thead() << std::endl
	 << cgicc::tr()    << std::endl //open
	 << cgicc::th()    << "Status" << cgicc::th() << std::endl
	 << cgicc::th()    << "Value"  << cgicc::th() << std::endl
	 << cgicc::tr()    << std::endl //close
	 << cgicc::thead() << std::endl 
      
	 << "<tbody>" << std::endl

	 << "<tr>" << std::endl
	 << "<td>" << "is_working_" << "</td>"
	 << "<td>" << is_working_   << "</td>"
	 << "</tr>"   << std::endl

	 << "<tr>" << std::endl
	 << "<td>" << "is_initialized_" << "</td>"
	 << "<td>" << is_initialized_   << "</td>"
	 << "</tr>"       << std::endl

	 << "<tr>" << std::endl
	 << "<td>" << "is_configured_" << "</td>"
	 << "<td>" << is_configured_   << "</td>"
	 << "</tr>"      << std::endl

	 << "<tr>" << std::endl
	 << "<td>" << "is_running_" << "</td>"
	 << "<td>" << is_running_   << "</td>"
	 << "</tr>"   << std::endl

	 << "</tbody>" << std::endl
	 << "</table>" << cgicc::br() << std::endl
	 << "</td>"    << std::endl;
    
    *out  << "<td>"     << std::endl
	  << "<table class=\"xdaq-table\">" << std::endl
	  << cgicc::thead() << std::endl
	  << cgicc::tr()    << std::endl //open
	  << cgicc::th()    << "Device"     << cgicc::th() << std::endl
	  << cgicc::th()    << "Connected"  << cgicc::th() << std::endl
	  << cgicc::tr()    << std::endl //close
	  << cgicc::thead() << std::endl 
	  << "<tbody>" << std::endl;
    
    *out << "</tbody>" << std::endl
	 << "</table>" << std::endl
	 << "</td>"    << std::endl
	 << "</tr>"    << std::endl
	 << "</tbody>" << std::endl
	 << "</table>" << std::endl;
    //<< "</div>"   << std::endl;

    *out << cgicc::script().set("type","text/javascript")
      .set("src","http://ajax.googleapis.com/ajax/libs/jquery/1/jquery.min.js")
	 << cgicc::script() << std::endl;
    *out << cgicc::script().set("type","text/javascript")
      .set("src","http://ajax.googleapis.com/ajax/libs/jqueryui/1/jquery-ui.min.js")
	 << cgicc::script() << std::endl;

  }// end try
  catch (const xgi::exception::Exception& e) {
    LOG4CPLUS_INFO(this->getApplicationLogger(),"Something went wrong displaying HVscan control panel(xgi): " << e.what());
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  catch (const std::exception& e) {
    LOG4CPLUS_INFO(this->getApplicationLogger(),"Something went wrong displaying HVscan control panel(std): " << e.what());
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
}


void gem::supervisor::tbutils::HVscan::webConfigure(xgi::Input *in, xgi::Output *out)
  throw (xgi::exception::Exception) {

  try {
    cgicc::Cgicc cgi(in);

    //sending SOAP message
    //    sendMessage(in,out);

    cgicc::form_iterator oh = cgi.getElement("SetChamber");
    if (strcmp((**oh).c_str(),"CH1Top") == 0) {
      confParams_.bag.chamber.toString()= "Chamber1Top";
      INFO("OH_0 has been selected " << confParams_.bag.chamber.toString());
    }//if Chamber1
    if (strcmp((**oh).c_str(),"CH1Bot") == 0) {
      confParams_.bag.chamber.toString()= "Chamber1Bottom";
      INFO("OH_0 has been selected " << confParams_.bag.chamber.toString());
    }//if Chamber1
    if (strcmp((**oh).c_str(),"CH2Top") == 0) {
      confParams_.bag.chamber.toString()= "Chamber1Top";
      INFO("OH_0 has been selected " << confParams_.bag.chamber.toString());
    }//if Chamber1
    if (strcmp((**oh).c_str(),"CH2Bot") == 0) {
      confParams_.bag.chamber.toString()= "Chamber1Bottom";
      INFO("OH_0 has been selected " << confParams_.bag.chamber.toString());
    }//if Chamber1
    if (strcmp((**oh).c_str(),"CH3Top") == 0) {
      confParams_.bag.chamber.toString()= "Chamber1Top";
      INFO("OH_0 has been selected " << confParams_.bag.chamber.toString());
    }//if Chamber1
    if (strcmp((**oh).c_str(),"CH3Bot") == 0) {
      confParams_.bag.chamber.toString()= "Chamber1Bottom";
      INFO("OH_0 has been selected " << confParams_.bag.chamber.toString());
    }//if Chamber1
    if (strcmp((**oh).c_str(),"CH4Top") == 0) {
      confParams_.bag.chamber.toString()= "Chamber1Top";
      INFO("OH_0 has been selected " << confParams_.bag.chamber.toString());
    }//if Chamber1
    if (strcmp((**oh).c_str(),"CH4Bot") == 0) {
      confParams_.bag.chamber.toString()= "Chamber1Bottom";
      INFO("OH_0 has been selected " << confParams_.bag.chamber.toString());
    }//if Chamber1

    
    //aysen's xml parser
    confParams_.bag.settingsFile = cgi.getElement("xmlFilename")->getValue();
    
    cgicc::const_form_iterator element = cgi.getElement("HV");
    if (element != cgi.getElements().end())
      scanParams_.bag.minHV   = element->getIntegerValue();

    element = cgi.getElement("MinHV");
    if (element != cgi.getElements().end())
      scanParams_.bag.minHV = element->getIntegerValue();
    
    element = cgi.getElement("MaxHV");
    if (element != cgi.getElements().end())
      scanParams_.bag.maxHV = element->getIntegerValue();

    element = cgi.getElement("VStep");
    if (element != cgi.getElements().end())
      scanParams_.bag.stepSize  = element->getIntegerValue();
        
    element = cgi.getElement("NTrigsStep");
    if (element != cgi.getElements().end())
      confParams_.bag.nTriggers  = element->getIntegerValue();
  }
  catch (const xgi::exception::Exception & e) {
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  catch (const std::exception & e) {
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  
  wl_->submit(confSig_);
  
  redirect(in,out);
}


void gem::supervisor::tbutils::HVscan::webStart(xgi::Input *in, xgi::Output *out)
  throw (xgi::exception::Exception) {

  try {
    cgicc::Cgicc cgi(in);
    
    cgicc::const_form_iterator element = cgi.getElement("HV");
    if (element != cgi.getElements().end())
      scanParams_.bag.minHV   = element->getIntegerValue();

    element = cgi.getElement("MinHV");
    if (element != cgi.getElements().end())
      scanParams_.bag.minHV = element->getIntegerValue();
    
    element = cgi.getElement("MaxHV");
    if (element != cgi.getElements().end())
      scanParams_.bag.maxHV = element->getIntegerValue();

    element = cgi.getElement("VStep");
    if (element != cgi.getElements().end())
      scanParams_.bag.stepSize  = element->getIntegerValue();
        
    element = cgi.getElement("NTrigsStep");
    if (element != cgi.getElements().end())
      confParams_.bag.nTriggers  = element->getIntegerValue();
  }
  catch (const xgi::exception::Exception & e) {
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  catch (const std::exception & e) {
    XCEPT_RAISE(xgi::exception::Exception, e.what());
  }
  
  wl_->submit(startSig_);
  
  redirect(in,out);
}

// State transitions
void gem::supervisor::tbutils::HVscan::configureAction(toolbox::Event::Reference e)
  throw (toolbox::fsm::exception::Exception) {

  is_working_ = true;

  //--------------------AMC13 Configure --------------

  nTriggers_ = confParams_.bag.nTriggers;
  stepSize_  = scanParams_.bag.stepSize;
  minHV_ = scanParams_.bag.minHV;
  maxHV_ = scanParams_.bag.maxHV;

  NTriggersAMC13();
  sendConfigureMessageAMC13();
  sendConfigureMessageGLIB();
  
  hw_semaphore_.take();

  confParams_.bag.triggersSeen = 0;
  totaltriggers = 0;
  confParams_.bag.triggercount = 0;
  
  for (auto chip = vfatDevice_.begin(); chip != vfatDevice_.end(); ++chip) {
    (*chip)->setRunMode(0);


    LOG4CPLUS_INFO(getApplicationLogger(),"loading default settings");
    //default settings for the frontend
    (*chip)->setTriggerMode(    0x3); //set to S1 to S8
    (*chip)->setCalibrationMode(0x0); //set to normal
    (*chip)->setMSPolarity(     0x1); //negative
    (*chip)->setCalPolarity(    0x1); //negative
    
    (*chip)->setProbeMode(        0x0);
    (*chip)->setLVDSMode(         0x0);
    (*chip)->setDACMode(          0x0);
    (*chip)->setHitCountCycleTime(0x0); //maximum number of bits
    
    (*chip)->setHitCountMode( 0x0);
    (*chip)->setMSPulseLength(0x3);
    (*chip)->setInputPadMode( 0x0);
    (*chip)->setTrimDACRange( 0x0);
    (*chip)->setBandgapPad(   0x0);
    (*chip)->sendTestPattern( 0x0);
        
    (*chip)->setIPreampIn(  168);
    (*chip)->setIPreampFeed(150);
    (*chip)->setIPreampOut(  80);
    (*chip)->setIShaper(    150);
    (*chip)->setIShaperFeed(100);
    (*chip)->setIComp(      120);

  }

  // flush FIFO, how to disable a specific, misbehaving, chip
  INFO("Flushing the FIFOs, readout_mask 0x" <<std::hex << (int)readout_mask << std::dec);
  DEBUG("Flushing FIFO" << readout_mask << " (depth " << glibDevice_->getFIFOOccupancy(readout_mask));
  glibDevice_->flushFIFO(readout_mask);
  while (glibDevice_->hasTrackingData(readout_mask)) {
    glibDevice_->flushFIFO(readout_mask);
    std::vector<uint32_t> dumping = glibDevice_->getTrackingData(readout_mask,
								 glibDevice_->getFIFOVFATBlockOccupancy(readout_mask));
  }
    // once more for luck
  glibDevice_->flushFIFO(readout_mask);

  //reset counters
  optohybridDevice_->resetL1ACount(0x5);
  optohybridDevice_->resetResyncCount();
  optohybridDevice_->resetBC0Count();
  optohybridDevice_->resetCalPulseCount(0x1);
  optohybridDevice_->sendResync();     
  optohybridDevice_->sendBC0();          

  is_configured_ = true;
  hw_semaphore_.give();

  is_working_    = false;
}


void gem::supervisor::tbutils::HVscan::startAction(toolbox::Event::Reference e)
  throw (toolbox::fsm::exception::Exception) {
  
  is_working_ = true;
  sendStartMessageGLIB();
  sendStartMessageAMC13();
  sleep(1);

  //AppHeader set;
  nTriggers_ = confParams_.bag.nTriggers;
  stepSize_  = scanParams_.bag.stepSize;
  minHV_ = scanParams_.bag.minHV;
  maxHV_ = scanParams_.bag.maxHV;

  //char data[128/8]
  is_running_ = true;
  hw_semaphore_.take();
  
  //set trigger source
  optohybridDevice_->setTrigSource(0x0);// trigger sources   

  //flush fifo
  INFO("Flushing the FIFOs, readout_mask 0x" <<std::hex << (int)readout_mask << std::dec);
  DEBUG("Flushing FIFO" << readout_mask << " (depth " << glibDevice_->getFIFOOccupancy(readout_mask));
  glibDevice_->flushFIFO(readout_mask);
  while (glibDevice_->hasTrackingData(readout_mask)) {
    glibDevice_->flushFIFO(readout_mask);
    std::vector<uint32_t> dumping = glibDevice_->getTrackingData(readout_mask,
                                                                 glibDevice_->getFIFOVFATBlockOccupancy(readout_mask));
  }
  // once more for luck
  glibDevice_->flushFIFO(readout_mask);
  
  optohybridDevice_->sendResync();      
  optohybridDevice_->sendBC0();          

  glibDevice_->setDAQLinkRunType(1);
  glibDevice_->setDAQLinkRunParameter(1,scanParams_.bag.minHV);


  //reset counters
  optohybridDevice_->resetL1ACount(0x0);

  wl_->submit(runSig_);

  hw_semaphore_.give();
  //start scan routine
  
  is_working_ = false;
}


void gem::supervisor::tbutils::HVscan::resetAction(toolbox::Event::Reference e)
  throw (toolbox::fsm::exception::Exception) {

  is_working_ = true;
  gem::supervisor::tbutils::GEMTBUtil::resetAction(e);
  
  scanParams_.bag.minHV     =    0;
  scanParams_.bag.maxHV     =  100;
  scanParams_.bag.stepSize  =  10U;
  is_working_     = false;
}

//void gem::supervisor::tbutils::HVscan::sendMessage(xgi::Input *in, xgi::Output *out)

void gem::supervisor::tbutils::HVscan::sendConfigureMessageGLIB()
  throw (xgi::exception::Exception) {
  //  is_working_ = true;

  xoap::MessageReference msg = xoap::createMessage();
  xoap::SOAPPart soap = msg->getSOAPPart();
  xoap::SOAPEnvelope envelope = soap.getEnvelope();
  xoap::SOAPBody body = envelope.getBody();
  //  xoap::SOAPName command = envelope.createName("CallBackConfigure","xdaq", "urn:xdaq-soap:3.0");
  xoap::SOAPName command = envelope.createName("Configure","xdaq", "urn:xdaq-soap:3.0");
  body.addBodyElement(command);

  try 
    {
      xdaq::ApplicationDescriptor * d = getApplicationContext()->getDefaultZone()->getApplicationDescriptor("gem::hw::glib::GLIBManager", 4);
      xdaq::ApplicationDescriptor * o = this->getApplicationDescriptor();
      xoap::MessageReference reply = getApplicationContext()->postSOAP(msg, *o,  *d);
    }
  catch (xdaq::exception::Exception& e)
    {
      LOG4CPLUS_INFO(getApplicationLogger(),"------------------Fail sending configure message " << e.what());
      XCEPT_RETHROW (xgi::exception::Exception, "Cannot send message", e);
    }
  //  this->Default(in,out);
  LOG4CPLUS_INFO(getApplicationLogger(),"-----------The message to configure has been sent------------");
}      


bool gem::supervisor::tbutils::HVscan::sendStartMessageGLIB()
  throw (xgi::exception::Exception) {

  //  this->Default(in,out);
  LOG4CPLUS_INFO(getApplicationLogger(),"-----------The message to GLIB start sent------------");

  //  is_working_ = true;
  xoap::MessageReference msg = xoap::createMessage();
  xoap::SOAPPart soap = msg->getSOAPPart();
  xoap::SOAPEnvelope envelope = soap.getEnvelope();
  xoap::SOAPBody body = envelope.getBody();
  //  xoap::SOAPName command = envelope.createName("CallBackStart","xdaq", "urn:xdaq-soap:3.0");
  xoap::SOAPName command = envelope.createName("Start","xdaq", "urn:xdaq-soap:3.0");
  body.addBodyElement(command);

  try 
    {
      xdaq::ApplicationDescriptor * d = getApplicationContext()->getDefaultZone()->getApplicationDescriptor("gem::hw::glib::GLIBManager", 4);
      xdaq::ApplicationDescriptor * o = this->getApplicationDescriptor();
      xoap::MessageReference reply = getApplicationContext()->postSOAP(msg, *o,  *d);
      LOG4CPLUS_INFO(getApplicationLogger(),"-----------The message to start GLIB has been sent------------");
      return true;
    }
  catch (xdaq::exception::Exception& e)
    {
      LOG4CPLUS_INFO(getApplicationLogger(),"------------------Fail sending start message " << e.what());
      XCEPT_RETHROW (xgi::exception::Exception, "Cannot send message", e);
      return false;
    }
}      

void gem::supervisor::tbutils::HVscan::NTriggersAMC13()
  throw (xgi::exception::Exception) {
  //  is_working_ = true;

  LOG4CPLUS_INFO(getApplicationLogger(),"-----------start SOAP message modify paramteres AMC13------ ");

  xdaq::ApplicationDescriptor * d = getApplicationContext()->getDefaultZone()->getApplicationDescriptor("gem::hw::amc13::AMC13Manager", 3);
  xdaq::ApplicationDescriptor * o = this->getApplicationDescriptor();
  std::string    appUrn   = "urn:xdaq-application:"+d->getClassName();

  xoap::MessageReference msg_2 = xoap::createMessage();
  xoap::SOAPPart soap_2 = msg_2->getSOAPPart();
  xoap::SOAPEnvelope envelope_2 = soap_2.getEnvelope();
  xoap::SOAPName     parameterset   = envelope_2.createName("ParameterSet","xdaq",XDAQ_NS_URI);

  xoap::SOAPElement  container = envelope_2.getBody().addBodyElement(parameterset);
  container.addNamespaceDeclaration("xsd","http://www.w3.org/2001/XMLSchema");
  container.addNamespaceDeclaration("xsi","http://www.w3.org/2001/XMLSchema-instance");
  //  container.addNamespaceDeclaration("parameterset","http://schemas.xmlsoap.org/soap/encoding/");
  xoap::SOAPName tname_param    = envelope_2.createName("type","xsi","http://www.w3.org/2001/XMLSchema-instance");
  xoap::SOAPName pboxname_param = envelope_2.createName("Properties","props",appUrn);
  xoap::SOAPElement pbox_param = container.addChildElement(pboxname_param);
  pbox_param.addAttribute(tname_param,"soapenc:Struct");

  xoap::SOAPName pboxname_amc13config = envelope_2.createName("amc13ConfigParams","props",appUrn);
  xoap::SOAPElement pbox_amc13config = pbox_param.addChildElement(pboxname_amc13config);
  pbox_amc13config.addAttribute(tname_param,"soapenc:Struct");
  
  xoap::SOAPName    soapName_l1A = envelope_2.createName("L1Aburst","props",appUrn);
  xoap::SOAPElement cs_l1A      = pbox_amc13config.addChildElement(soapName_l1A);
  cs_l1A.addAttribute(tname_param,"xsd:unsignedInt");
  cs_l1A.addTextNode(confParams_.bag.nTriggers.toString());

  
  std::string tool;
  xoap::dumpTree(msg_2->getSOAPPart().getEnvelope().getDOMNode(),tool);
  DEBUG("msg_2: " << tool);
  
  try 
    {
      DEBUG("trying to send parameters");
      xoap::MessageReference reply_2 = getApplicationContext()->postSOAP(msg_2, *o,  *d);
      std::string tool;
      xoap::dumpTree(reply_2->getSOAPPart().getEnvelope().getDOMNode(),tool);
      DEBUG("reply_2: " << tool);
    }
  catch (xoap::exception::Exception& e)
    {
      LOG4CPLUS_ERROR(getApplicationLogger(),"------------------Fail  AMC13 configuring parameters message " << e.what());
      XCEPT_RETHROW (xoap::exception::Exception, "Cannot send message", e);
    }
  catch (xdaq::exception::Exception& e)
    {
      LOG4CPLUS_ERROR(getApplicationLogger(),"------------------Fail  AMC13 configuring parameters message " << e.what());
      XCEPT_RETHROW (xoap::exception::Exception, "Cannot send message", e);
    }
  catch (std::exception& e)
    {
      LOG4CPLUS_ERROR(getApplicationLogger(),"------------------Fail  AMC13 configuring parameters message " << e.what());
      //XCEPT_RETHROW (xoap::exception::Exception, "Cannot send message", e);
    }
  catch (...)
    {
      LOG4CPLUS_ERROR(getApplicationLogger(),"------------------Fail  AMC13 configuring parameters message ");
      XCEPT_RAISE (xoap::exception::Exception, "Cannot send message");
    }

  //  this->Default(in,out);
  LOG4CPLUS_INFO(getApplicationLogger(),"-----------The message to AMC13 configuring parameters has been sent------------");
}      


void gem::supervisor::tbutils::HVscan::sendConfigureMessageAMC13()
  throw (xgi::exception::Exception) {
  //  is_working_ = true;

  LOG4CPLUS_INFO(getApplicationLogger(),"-----------start SOAP message configuring AMC13------ ");

  xoap::MessageReference msg = xoap::createMessage();
  xoap::SOAPPart soap = msg->getSOAPPart();
  xoap::SOAPEnvelope envelope = soap.getEnvelope();
  xoap::SOAPBody body = envelope.getBody();
  xoap::SOAPName command = envelope.createName("Configure","xdaq", "urn:xdaq-soap:3.0");
  body.addBodyElement(command);

  xdaq::ApplicationDescriptor * d = getApplicationContext()->getDefaultZone()->getApplicationDescriptor("gem::hw::amc13::AMC13Manager", 3);
  xdaq::ApplicationDescriptor * o = this->getApplicationDescriptor();
  std::string    appUrn   = "urn:xdaq-application:"+d->getClassName();
  try 
    {
      xoap::MessageReference reply = getApplicationContext()->postSOAP(msg, *o,  *d);
      std::string tool;
      DEBUG("dumpTree(reply)");
      xoap::dumpTree(reply->getSOAPPart().getEnvelope().getDOMNode(),tool);
      DEBUG("reply: " << tool);
    }
  catch (xdaq::exception::Exception& e)
    {
      LOG4CPLUS_INFO(getApplicationLogger(),"------------------Fail sending AMC13 configure message " << e.what());
      XCEPT_RETHROW (xgi::exception::Exception, "Cannot send message", e);
    }
  //  this->Default(in,out);
  LOG4CPLUS_INFO(getApplicationLogger(),"-----------The message to AMC13 configure has been sent------------");


}      





bool gem::supervisor::tbutils::HVscan::sendStartMessageAMC13()
  throw (xgi::exception::Exception) {
  //  is_working_ = true;
  xoap::MessageReference msg = xoap::createMessage();
  xoap::SOAPPart soap = msg->getSOAPPart();
  xoap::SOAPEnvelope envelope = soap.getEnvelope();
  xoap::SOAPBody body = envelope.getBody();
  xoap::SOAPName command = envelope.createName("Start","xdaq", "urn:xdaq-soap:3.0");
  //  xoap::SOAPName command2 = envelope.createName("sendStartMessageAMC13","xdaq", "urn:xdaq-soap:3.0");
  
  body.addBodyElement(command);
  //  body.addBodyElement(command2);

  try 
    {
      xdaq::ApplicationDescriptor * d = getApplicationContext()->getDefaultZone()->getApplicationDescriptor("gem::hw::amc13::AMC13Manager", 3);
      xdaq::ApplicationDescriptor * o = this->getApplicationDescriptor();
      xoap::MessageReference reply = getApplicationContext()->postSOAP(msg, *o,  *d);
      
      LOG4CPLUS_INFO(getApplicationLogger(),"-----------The message to start the AMC13 has been sent------------");

      return true;
    }
  catch (xdaq::exception::Exception& e)
    {
      LOG4CPLUS_INFO(getApplicationLogger(),"------------------Fail sending AMC13 start message " << e.what());
      XCEPT_RETHROW (xgi::exception::Exception, "Cannot send message", e);
        return false;
    }
  //  this->Default(in,out);
}      

void gem::supervisor::tbutils::HVscan::sendAMC13trigger()
  throw (xgi::exception::Exception) {
  //  is_working_ = true;
  xoap::MessageReference msg = xoap::createMessage();
  xoap::SOAPPart soap = msg->getSOAPPart();
  xoap::SOAPEnvelope envelope = soap.getEnvelope();
  xoap::SOAPBody body = envelope.getBody();
  xoap::SOAPName command = envelope.createName("sendtriggerburst","xdaq", "urn:xdaq-soap:3.0");

  body.addBodyElement(command);

  try 
    {
      xdaq::ApplicationDescriptor * d = getApplicationContext()->getDefaultZone()->getApplicationDescriptor("gem::hw::amc13::AMC13Manager", 3);
      xdaq::ApplicationDescriptor * o = this->getApplicationDescriptor();
      xoap::MessageReference reply = getApplicationContext()->postSOAP(msg, *o,  *d);
      
      LOG4CPLUS_INFO(getApplicationLogger(),"-----------The message to start sending burst-----------");

    }
  catch (xdaq::exception::Exception& e)
    {
      LOG4CPLUS_INFO(getApplicationLogger(),"------------------Fail sending burst message " << e.what());
      XCEPT_RETHROW (xgi::exception::Exception, "Cannot send message", e);
    }
  //  this->Default(in,out);
}      

void gem::supervisor::tbutils::HVscan::selectCHAMBER(xgi::Output *out)
  throw (xgi::exception::Exception)
{
  try {
    bool isDisabled = false;
    if (is_running_ || is_configured_ || is_initialized_)
      isDisabled = true;
    
    // cgicc::input OHselection;
    if (isDisabled)
      *out << cgicc::select().set("name","SetChamber").set("disabled","disabled") 
	   << cgicc::option("CH1Top").set("value","Chamber1Top")
	   << cgicc::option("CH1Bot").set("value","Chamber1Bottom")
	   << cgicc::option("CH2Top").set("value","Chamber2Top")
	   << cgicc::option("CH2Bot").set("value","Chamber2Bottom")
	   << cgicc::option("CH3Top").set("value","Chamber3Top")
	   << cgicc::option("CH3Bot").set("value","Chamber3Bottom")
	   << cgicc::option("CH4Top").set("value","Chamber4Top")
	   << cgicc::option("CH4Bot").set("value","Chamber4Bottom")
	   << cgicc::select().set("disabled","disabled") << std::endl
	   << "</td>"    << std::endl
	   << "</tr>"    << std::endl
	   << "</table>" << std::endl;
    else
      *out << cgicc::select().set("name","SetChamber") << std::endl
	   << cgicc::option("CH1Top").set("value","Chamber1Top")
	   << cgicc::option("CH1Bot").set("value","Chamber1Bottom")
	   << cgicc::option("CH2Top").set("value","Chamber2Top")
	   << cgicc::option("CH2Bot").set("value","Chamber2Bottom")
	   << cgicc::option("CH3Top").set("value","Chamber3Top")
	   << cgicc::option("CH3Bot").set("value","Chamber3Bottom")
	   << cgicc::option("CH4Top").set("value","Chamber4Top")
	   << cgicc::option("CH4Bot").set("value","Chamber4Bottom")
	   << cgicc::select()<< std::endl
	   << "</td>"    << std::endl
	   << "</tr>"    << std::endl
	   << "</table>" << std::endl;
	
    /*      *out << "<tr><td class=\"title\"> Select Latency Scan: </td>"
	    << "<td class=\"form\">"*/
    
  }//end try
catch (const xgi::exception::Exception& e) {
  INFO("Something went wrong setting the trigger source): " << e.what());
  XCEPT_RAISE(xgi::exception::Exception, e.what());
 }
 catch (const std::exception& e) {
   INFO("Something went wrong setting the trigger source): " << e.what());
   XCEPT_RAISE(xgi::exception::Exception, e.what());
 }

}// end void selectoptohybrid



void gem::supervisor::tbutils::HVscan::setHVvalue()
  throw (xgi::exception::Exception) {
  //  is_working_ = true;
  
  LOG4CPLUS_INFO(getApplicationLogger(),"-----------start SOAP message to SET HV------ ");

  xdaq::ApplicationDescriptor * d = getApplicationContext()->getDefaultZone()->getApplicationDescriptor("gem::hw::amc13::AMC13Manager", 3);
  xdaq::ApplicationDescriptor * o = this->getApplicationDescriptor();
  std::string    appUrn   = "urn:xdaq-application:"+d->getClassName();

  xoap::MessageReference msg_2 = xoap::createMessage();
  xoap::SOAPPart soap_2 = msg_2->getSOAPPart();
  xoap::SOAPEnvelope envelope_2 = soap_2.getEnvelope();
  xoap::SOAPName     parameterset   = envelope_2.createName("ParameterSet","xdaq",XDAQ_NS_URI);

  xoap::SOAPElement  container = envelope_2.getBody().addBodyElement(parameterset);
  container.addNamespaceDeclaration("xsd","http://www.w3.org/2001/XMLSchema");
  container.addNamespaceDeclaration("xsi","http://www.w3.org/2001/XMLSchema-instance");
  //  container.addNamespaceDeclaration("parameterset","http://schemas.xmlsoap.org/soap/encoding/");
  xoap::SOAPName tname_param    = envelope_2.createName("type","xsi","http://www.w3.org/2001/XMLSchema-instance");
  xoap::SOAPName pboxname_param = envelope_2.createName("Properties","props",appUrn);
  xoap::SOAPElement pbox_param = container.addChildElement(pboxname_param);
  pbox_param.addAttribute(tname_param,"soapenc:Struct");

  xoap::SOAPName pboxname_amc13config = envelope_2.createName("amc13ConfigParams","props",appUrn);
  xoap::SOAPElement pbox_amc13config = pbox_param.addChildElement(pboxname_amc13config);
  pbox_amc13config.addAttribute(tname_param,"soapenc:Struct");
  
  xoap::SOAPName    soapName_l1A = envelope_2.createName("L1Aburst","props",appUrn);
  xoap::SOAPElement cs_l1A      = pbox_amc13config.addChildElement(soapName_l1A);
  cs_l1A.addAttribute(tname_param,"xsd:unsignedInt");
  cs_l1A.addTextNode(confParams_.bag.nTriggers.toString());

  
  std::string tool;
  xoap::dumpTree(msg_2->getSOAPPart().getEnvelope().getDOMNode(),tool);
  DEBUG("msg_2: " << tool);
  
  try 
    {
      DEBUG("trying to send parameters");
      xoap::MessageReference reply_2 = getApplicationContext()->postSOAP(msg_2, *o,  *d);
      std::string tool;
      xoap::dumpTree(reply_2->getSOAPPart().getEnvelope().getDOMNode(),tool);
      DEBUG("reply_2: " << tool);
    }
  catch (xoap::exception::Exception& e)
    {
      LOG4CPLUS_ERROR(getApplicationLogger(),"------------------Fail  AMC13 configuring parameters message " << e.what());
      XCEPT_RETHROW (xoap::exception::Exception, "Cannot send message", e);
    }
  catch (xdaq::exception::Exception& e)
    {
      LOG4CPLUS_ERROR(getApplicationLogger(),"------------------Fail  AMC13 configuring parameters message " << e.what());
      XCEPT_RETHROW (xoap::exception::Exception, "Cannot send message", e);
    }
  catch (std::exception& e)
    {
      LOG4CPLUS_ERROR(getApplicationLogger(),"------------------Fail  AMC13 configuring parameters message " << e.what());
      //XCEPT_RETHROW (xoap::exception::Exception, "Cannot send message", e);
    }
  catch (...)
    {
      LOG4CPLUS_ERROR(getApplicationLogger(),"------------------Fail  AMC13 configuring parameters message ");
      XCEPT_RAISE (xoap::exception::Exception, "Cannot send message");
    }

  //  this->Default(in,out);
  LOG4CPLUS_INFO(getApplicationLogger(),"-----------The message to AMC13 configuring parameters has been sent------------");
}      



  /*
  xdaq::ApplicationDescriptor * d = getApplicationContext()->getDefaultZone()->getApplicationDescriptor("pvss", 0);
  xdaq::ApplicationDescriptor * o = this->getApplicationDescriptor();
  std::string    appUrn   = "urn:xdaq-application:"+d->getClassName();
  

  xoap::MessageReference msg = xoap::createMessage();
  xoap::SOAPEnvelope env = msg->getEnvelope();
  xoap::SOAPBody body = env.getBody();
  xoap::SOAPName cmdName = env.createName("dpSet","psx",appUrn);
  xoap::SOAPBodyElement bodyElem = body.addBodyElement(cmdName);
  // add data point name: <psx::dp name="name">value</psx::dp>                                                                                                                                                                              
  xoap::SOAPName dpName =  env.createName("dp","psx",appUrn);
  xoap::SOAPElement dpElement = bodyElem.addChildElement(dpName);
  xoap::SOAPName nameAttribute = env.createName("name","","");
  dpElement.addAttribute(nameAttribute, dp);
  dpElement.addTextNode(value);

  try
      {

	xoap::MessageReference reply = getApplicationContext()->postSOAP(msg, d);
	// extract the result from elements in reply message
      }
    catch(xoap::exception::Exception& e)
      {
	LOG4CPLUS_ERROR(getApplicationLogger(),"------------------Fail  Setting HV  " << e.what());
	XCEPT_RETHROW (xoap::exception::Exception, "Cannot send message", e);
    }

  //  this->Default(in,out);
  LOG4CPLUS_INFO(getApplicationLogger(),"-----------HV was set------------");
  }   */  

