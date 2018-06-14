/* Radio */

// TODO: there are lots more frequencies than this. pick a good one from the sd card and use constrain()
#define RADIO_FREQ 915.0

RH_RF95 rf95(RFM95_CS, RFM95_INT);

void setupRadio() {
  DEBUG_PRINT(F("Setting up Radio... "));

  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);

  delay(100); // give the radio time to wake up

  // manual reset
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  // Defaults after init are 434.0MHz, modulation GFSK_Rb250Fd250, 13dBm, Bw = 125 kHz, Cr = 4/5, Sf = 128chips/symbol,
  // CRC on
  if (!rf95.init()) {
    DEBUG_PRINTLN(F("failed! Cannot proceed without Radio!"));
    while (1)
      ;
  }

  // we could read this from the SD card, but I think 868 requires a license
  if (!rf95.setFrequency(RADIO_FREQ)) {
    DEBUG_PRINTLN(F("setFrequency failed! Cannot proceed!"));
    while (1)
      ;
  }
  DEBUG_PRINT(F("Freq: "));
  DEBUG_PRINT(RADIO_FREQ);

  // The default transmitter power is 13dBm, using PA_BOOST.
  rf95.setTxPower(constrain(radio_power, 5, 23), false);

  DEBUG_PRINTLN(F(" done."));
}

//long wait_for_congestion = 0;

void radioTransmit(int pid) {
  static uint8_t radio_buf[RH_RF95_MAX_MESSAGE_LEN];

  if (timeStatus() == timeNotSet) {
    DEBUG_PRINTLN(F("Time not set! Skipping transmission."));
    return;
  }

  unsigned long time_now = now();

  // TODO: put this 2 second wait on the SD card. maybe tie it to update interval?
  // TODO: what if time_now/wait_for_congestion wraps?
  //if ((time_now - last_transmitted[pid] < 2) or (time_now < wait_for_congestion)) {
  if (time_now - last_transmitted[pid] < 2) {
    // we already transmitted for this peer recently. skip it
    return;
  }

  // TODO: should we transmit peer data even if we don't have local time set?
  // maybe set time from a peer?
  DEBUG_PRINT(F("My time to transmit ("));
  DEBUG_PRINT(time_now);
  DEBUG_PRINTLN(F(")... "));

  /*
  // TODO: this is causing it to hang. does my module not have this?
  // http://www.airspayce.com/mikem/arduino/RadioHead/classRHGenericDriver.html#ac577b932ba8b042b8170b24d513635c7
  if (rf95.isChannelActive()) {
    DEBUG_PRINTLN("Channel is active. Delaying transmission");
    // TODO: exponential backoff? how long? FastLED's random?
    // TODO: how long should we wait? the upstream method waits 100-1000ms
    wait_for_congestion = time_now + random(100, 500);
    return;
  }
  */

  if (rf95.available()) {
    DEBUG_PRINTLN("Missed a peer message! Parsing before transmitting.");
    radioReceive();
    // TODO: do something with the lights? could be cool to add a circle in my_hue to whatever pattern is currently playing
    return;  // we will try broadcasting next loop
  }

  if (!compass_messages[pid].hue or (pid == my_peer_id and !GPS.fix)) {
    // if we don't have any info for this peer, skip sending anything

    // don't bother retrying
    last_transmitted[pid] = time_now;

    // TODO: this blocks us from being able to use pure red
    DEBUG_PRINT("No peer data to transmit for #");
    DEBUG_PRINTLN(pid);

    return;
  }

  // TODO: what to do if the message is really old?
  compass_messages[pid].tx_time = time_now;
  compass_messages[pid].tx_ms = network_ms;

  // DEBUGGING
  DEBUG_PRINT(F("Message: n="));
  DEBUG_PRINT(compass_messages[pid].network_id);
  DEBUG_PRINT(F(" txp="));
  DEBUG_PRINT(compass_messages[pid].tx_peer_id);
  DEBUG_PRINT(F(" p="));
  DEBUG_PRINT(compass_messages[pid].peer_id);
  DEBUG_PRINT(F(" now="));
  DEBUG_PRINT(compass_messages[pid].tx_time);
  DEBUG_PRINT(F(" ms="));
  DEBUG_PRINT(compass_messages[pid].tx_ms);
  DEBUG_PRINT(F(" t="));
  DEBUG_PRINT(compass_messages[pid].last_updated_at);
  DEBUG_PRINT(F(" lat="));
  DEBUG_PRINT(compass_messages[pid].latitude);
  DEBUG_PRINT(F(" lon="));
  DEBUG_PRINT(compass_messages[pid].longitude);
  DEBUG_PRINT(F(" EOM. "));

  // Create a stream that will write to our buffer
  pb_ostream_t stream = pb_ostream_from_buffer(radio_buf, sizeof(radio_buf));
  // TODO: max size (SmartCompassMessage_size) is only 64 bytes. we could combine 3 of them into one packet

  if (!pb_encode(&stream, SmartCompassMessage_fields, &compass_messages[pid])) {
    DEBUG_PRINTLN(F("ERROR ENCODING!"));
    return;
  }

  // sending will wait for any previous send with waitPacketSent(), but we want to dither LEDs. transmitting is fast (TODO: time it)
  DEBUG_PRINT(F("sending... "));
  rf95.send((uint8_t *)radio_buf, stream.bytes_written);
  while (rf95.mode() == RH_RF95_MODE_TX) {
    FastLED.delay(2);
  }

  // put the radio to sleep to save power
  // TODO: this takes a finite amount of time to wake. not sure how long tho...
  rf95.sleep();

  last_transmitted[pid] = time_now;

  DEBUG_PRINTLN(F("done."));
  return;
}

void radioReceive() {
  // i had separate buffers for tx and for rx, but that doesn't seem necessary
  static uint8_t radio_buf[RH_RF95_MAX_MESSAGE_LEN]; // TODO: keep this off the stack
  static uint8_t radio_buf_len;
  static SmartCompassMessage message = SmartCompassMessage_init_default;

  // DEBUG_PRINTLN("Checking for reply...");
  if (rf95.available()) {
    // radio_buf_len = sizeof(radio_buf);  // TODO: protobuf uses size_t, but radio uses uint8_t
    radio_buf_len = RH_RF95_MAX_MESSAGE_LEN; // reset this to max length otherwise it won't receive the full message!

    // Should be a reply message for us now
    if (rf95.recv(radio_buf, &radio_buf_len)) {
      DEBUG_PRINT(F("RSSI: "));
      DEBUG_PRINTLN2(rf95.lastRssi(), DEC);

      pb_istream_t stream = pb_istream_from_buffer(radio_buf, radio_buf_len);
      if (!pb_decode(&stream, SmartCompassMessage_fields, &message)) {
        DEBUG_PRINT(F("Decoding failed: "));
        DEBUG_PRINTLN(PB_GET_ERROR(&stream));
        return;
      }

      if (message.network_id != my_network_id) {
        DEBUG_PRINT(F("Message is for another network: "));
        DEBUG_PRINTLN(message.network_id);
        return;
      }

      if (message.tx_peer_id == my_peer_id) {
        DEBUG_PRINT(F("ERROR! Peer id collision! "));
        DEBUG_PRINTLN(my_peer_id);
        return;
      }

      if (message.peer_id == my_peer_id) {
        DEBUG_PRINTLN(F("Ignoring stats about myself."));
        // TODO: instead of ignoring, track how how my info is on all peers. if it is old, maybe there was some interference
        return;
      }

      if (message.last_updated_at < compass_messages[message.peer_id].last_updated_at) {
        DEBUG_PRINTLN(F("Ignoring old message."));
        return;
      }

      // TODO: make this work. somehow time got set to way in the future
      /*
      // sync to the lowest peer id's time
      // TODO: only do this if there is drift?
      // TODO: make sure this works well for all cases
      //if (message.peer_id < my_peer_id) {
      if (timeStatus() == timeNotSet) {
        setTime(0, 0, 0, 0, 0, 0);
        adjustTime(message.tx_time);
      }
      */

      if (message.peer_id < my_peer_id) {
        DEBUG_PRINT("Updating network_ms! ");
        DEBUG_PRINT(network_ms);
        DEBUG_PRINT(" -> ");
        network_ms = message.tx_ms + 74;  // TODO: tune this offset. probably save it as a global
      } else {
        DEBUG_PRINT("Leaving network_ms alone! ");
      }
      DEBUG_PRINTLN(network_ms);

      // TODO: do we care about saving tx times? we will change them when we re-broadcast
      //compass_messages[message.peer_id].tx_time = message.tx_time;
      //compass_messages[message.peer_id].tx_ms = message.tx_ms;

      compass_messages[message.peer_id].last_updated_at = message.last_updated_at;
      compass_messages[message.peer_id].hue = message.hue;
      compass_messages[message.peer_id].saturation = message.saturation;
      compass_messages[message.peer_id].latitude = message.latitude;
      compass_messages[message.peer_id].longitude = message.longitude;

      DEBUG_PRINT(F("Message for peer #"));
      DEBUG_PRINT(message.peer_id);
      DEBUG_PRINT(F(" from #"));
      DEBUG_PRINT(message.tx_peer_id);
      DEBUG_PRINT(F(": t="));
      DEBUG_PRINT(message.tx_time);
      DEBUG_PRINT(F(" ms="));
      DEBUG_PRINT(message.tx_ms);
      DEBUG_PRINT(F(" h="));
      DEBUG_PRINT(message.hue);
      DEBUG_PRINT(F(" s="));
      DEBUG_PRINT(message.saturation);
      DEBUG_PRINT(F(" lat="));
      DEBUG_PRINT(message.latitude);
      DEBUG_PRINT(F(" lon="));
      DEBUG_PRINTLN(message.longitude);
    } else {
      DEBUG_PRINTLN(F("Receive failed"));
    }
  }
}
