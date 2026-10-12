// Included by the actual-driver harness. No model of the production cache or
// conversion state machine is used here: only Wire, clock and mutex seams.
static void scanCase(const std::string& mode) {
    const char* scan = R"({"scanMask":240,"dataRate":128})";
    const auto* metadata = sensorDriverMetadata("ADS1115");
    ADS1115Driver driver;
    SensorReading reading;
    Wire.muxSamplesEnabled = true;
    auto& cache = SensorDriverCache::instance();
    float value = NAN; String unit, error;
    auto read = [&](const char* channel, SensorSampleMetadata* sample = nullptr) {
        return cache.readChannel("adc", "ADS1115", 0, scan, channel, value, unit, error,
            1000, nullptr, 0, nullptr, sample);
    };
    if (mode == "scan_config" || mode == "scan_masks") {
        for (unsigned mask = 0; mask <= 255; ++mask) {
            unsigned count = 0;
            for (unsigned bit = 0; bit < 8; ++bit) count += (mask >> bit) & 1;
            PeripheralConfig config; initializeSensorDefaults(config);
            config.pinCount = 2; config.pins[0] = 21; config.pins[1] = 22;
            JsonDocument input; input["driver"] = "ADS1115"; input["scanMask"] = mask;
            input["dataRate"] = 8; input["sampleInterval"] = 1200;
            const bool accepted = parseSensorConfig(input.as<JsonObjectConst>(), config, error);
            CHECK(accepted == (count <= 4));
            if (!accepted) { CHECK(config.params.sensor.scanMask == 0); continue; }
            CHECK(validateSensorConfig(config, error));
            const unsigned budget = (count ? count : 1) * 300;
            CHECK(sensorSampleIntervalMs(config) == 1200);
            config.params.sensor.sampleInterval = budget - 1;
            CHECK(!validateSensorConfig(config, error));
            config.params.sensor.sampleInterval = budget;
            CHECK(validateSensorConfig(config, error));
            CHECK(sensorSampleIntervalMs(config) == budget);
            JsonDocument output; serializeSensorConfig(output.to<JsonObject>(), config);
            CHECK(output["scanMask"].as<unsigned>() == mask);
            PeripheralConfig restored; initializeSensorDefaults(restored);
            restored.pinCount=2; restored.pins[0]=21; restored.pins[1]=22;
            CHECK(parseSensorConfig(output.as<JsonObjectConst>(), restored, error));
            CHECK(restored.params.sensor.scanMask == mask);
            JsonDocument params; buildSensorDriverParams(params, config, 21, 22, 100000);
            std::string encoded; serializeJson(params, encoded);
            CHECK(driver.init(0, encoded.c_str()));
            CHECK(driver.getChannelCount() == (mask ? count : 2));
            CHECK(metadata->activeChannelCount(0, mask) == driver.getChannelCount());
            CHECK(driver.getMinInterval() == budget);
            for (unsigned index=0; index<driver.getChannelCount(); ++index) {
                const auto* channel = metadata->getChannel(index, 0, mask); CHECK(channel);
                CHECK(!std::strcmp(driver.getChannelName(index), channel->name));
                CHECK(!std::strcmp(driver.getChannelUnit(index), channel->unit));
                String resolved;
                CHECK(resolveSensorChannel("ADS1115",0,channel->name,resolved,error,mask));
                CHECK(resolved == channel->name);
            }
            String resolved;
            CHECK(resolveSensorChannel("ADS1115",0,"rawCode",resolved,error,mask) == (mask == 0));
            if (mode == "scan_masks") {
                const unsigned starts = Wire.starts;
                CHECK(driver.read(reading) && reading.success);
                CHECK(Wire.starts - starts == (count ? count : 1));
                CHECK(reading.channelMetadata == (mask != 0));
                CHECK(reading.channelCount == driver.getChannelCount());
            }
            driver.deinit();
        }
        for (const char* invalid : {R"({"scanMask":256})",R"({"scanMask":-1})",
             R"({"scanMask":31})",R"({"scanMask":true})",R"({"scanMask":1.5})"}) {
            const unsigned transfers=Wire.transmissions; CHECK(!driver.init(0,invalid));
            CHECK(Wire.transmissions==transfers);
        }
        return;
    }
    if (mode == "scan_values" || mode == "scan_step_bound") {
        CHECK(driver.init(0,scan));
        if (mode == "scan_values") CHECK(driver.read(reading));
        else {
            SensorReadStep step;
            do {
                const unsigned transfers=Wire.transmissions, requests=Wire.requests;
                step=driver.readStep(reading);
                CHECK(Wire.transmissions-transfers <= 1 && Wire.requests-requests <= 1);
                if(step==SensorReadStep::Pending) delay(1);
            } while(step==SensorReadStep::Continue || step==SensorReadStep::Pending);
            CHECK(step==SensorReadStep::Complete);
        }
        CHECK(reading.success && reading.channelMetadata && reading.channelCount==4);
        CHECK(Wire.startedMuxes == (std::vector<unsigned>{4,5,6,7}));
        for(unsigned i=0;i<4;++i){
            CHECK(std::fabs(reading.values[i]-(i+1)*0.512f)<0.00001f);
            CHECK(reading.channelErrors[i]==SensorChannelError::None);
            CHECK(i==0 || reading.channelTimestamps[i]>reading.channelTimestamps[i-1]);
        }
        CHECK(reading.timestamp==reading.channelTimestamps[0]);
        driver.deinit(); return;
    }
    if (mode == "scan_faults") {
        CHECK(driver.init(0,scan));
        const unsigned before=Wire.transmissions; CHECK(driver.read(reading));
        const unsigned transfers=Wire.transmissions-before;
        driver.deinit();
        for(unsigned index=1;index<=transfers;++index){
            Wire=TwoWire(); Wire.muxSamplesEnabled=true;
            CHECK(driver.init(0,scan)); Wire.failTxAt=Wire.transmissions+index;
            CHECK(!driver.read(reading) && !reading.success && reading.channelMetadata);
            bool failed=false;
            for(unsigned i=0;i<reading.channelCount;++i){
                if(reading.channelErrors[i]==SensorChannelError::Transport){CHECK(!failed);failed=true;CHECK(std::isnan(reading.values[i]));}
                else if(failed) CHECK(reading.channelErrors[i]==SensorChannelError::NotAttempted && std::isnan(reading.values[i]));
            }
            CHECK(failed); driver.deinit();
        }
        return;
    }
    if (mode == "scan_protocol_failures") {
        for(const std::string fault : {"continuous","missed","lost","short","negative_read","short_write","lost_lease"}) {
            Wire=TwoWire();Wire.muxSamplesEnabled=true;
            if(fault=="continuous") Wire.config=0x0483;
            CHECK(driver.init(0,scan));
            if(fault=="missed") Wire.missedBusy=true;
            if(fault=="lost") Wire.configLost=true;
            if(fault=="short") Wire.shortRead=true;
            if(fault=="negative_read") Wire.negativeRead=true;
            if(fault=="short_write") Wire.writeLimit=0;
            if(fault=="lost_lease") CHECK(I2CBusManager::instance().release(&driver));
            const bool ok=driver.read(reading);
            CHECK(ok==(fault=="continuous"));
            if(ok) CHECK(reading.success && Wire.idleWrites==1);
            else CHECK(!reading.success && reading.channelMetadata && reading.channelErrors[0]!=SensorChannelError::None);
            driver.deinit();
        }
        return;
    }
    if (mode == "scan_fault_cache") {
        Wire.muxSamples[6]=0x7fff;
        CHECK(!read("voltage_mux4") && std::isnan(value));
        const unsigned starts=Wire.starts; CHECK(starts==3);
        CHECK(!read("voltage_mux7") && Wire.starts==starts);
        SensorReading snapshot;bool stale=false;
        CHECK(cache.getSnapshot("adc",snapshot,stale,error) && stale && !snapshot.success);
        CHECK(snapshot.channelCount==4 && snapshot.channelMetadata);
        CHECK(snapshot.channelErrors[0]==SensorChannelError::None);
        CHECK(snapshot.channelErrors[2]==SensorChannelError::Range);
        CHECK(snapshot.channelErrors[3]==SensorChannelError::NotAttempted);
        CHECK(std::isnan(snapshot.values[2]) && std::isnan(snapshot.values[3]));
        Wire.muxSamples[6]=0x3000; sensorTestNow+=1000;
        CHECK(read("voltage_mux4") && Wire.starts==starts+4);
    } else if (mode=="scan_cache" || mode=="scan_wrap") {
        uint32_t previous=0;
        if(mode=="scan_wrap"){
            sensorTestNow=0xfffffffdU; previous=sensorTestNow;
            scanHook=[&] { if(sensorTestNow<previous) sensorTestWrapBase+=UINT64_C(1)<<32; previous=sensorTestNow; };
            adsTestDelayHook=runScanHook;
        }
        SensorSampleMetadata first,last;uint32_t collection=sensorTestNow;
        CHECK(read("voltage_mux4",&first));
        const unsigned starts=Wire.starts;
        CHECK(cache.readChannel("adc","ADS1115",0,scan,"voltage_mux7",value,unit,error,
            1000,nullptr,0,&collection,&last));
        CHECK(starts==4 && Wire.starts==starts);
        CHECK(last.sampledAtMs>first.sampledAtMs && first.quality==SensorSampleQuality::Good);
        CHECK(first.sampleGroup && first.sampleGroup==last.sampleGroup);
        CHECK(last.sampledAtMs<=last.capturedAtMs);
        SensorReading snapshot;bool stale=false;
        CHECK(cache.getSnapshot("adc",snapshot,stale,error) && !stale && snapshot.success);
        CHECK(snapshot.channelMetadata && snapshot.channelCount==4);
        CHECK(first.sampledAtMs% (UINT64_C(1)<<32)==snapshot.channelTimestamps[0]);
        sensorTestNow+=1000; CHECK(read("voltage_mux7",&last) && Wire.starts==8);
        CHECK(last.sampleGroup>first.sampleGroup);
        adsTestDelayHook=nullptr;
    } else if (mode=="scan_release" || mode=="scan_replace" || mode=="scan_epoch" ||
               mode=="scan_cancel_all" || mode=="scan_parallel" || mode=="scan_lease") {
        CHECK(DriverRegistry::getInstance().registerDriver("ScanPeer",makeScanPeer));
        bool ran=false;
        scanHook=[&]{
            adsTestDelayHook=nullptr;ran=true;
            if(mode=="scan_parallel"){
                float peer;String peerUnit,peerError;
                const unsigned starts=Wire.starts;
                CHECK(!cache.readChannel("adc","ADS1115",0,scan,"voltage_mux7",peer,peerUnit,peerError,1000));
                CHECK(peerError=="SENSOR_DRIVER_BUSY" && Wire.starts==starts);
                CHECK(cache.readChannel("peer","ScanPeer",0,"{}","value",peer,peerUnit,peerError,1000) && peer==42);
            } else if(mode=="scan_lease") {
                ADS1115Driver duplicate;const unsigned transfers=Wire.transmissions;
                CHECK(!duplicate.init(0,scan) && Wire.transmissions==transfers);
            } else if(mode=="scan_epoch") {
                SensorConfigEpoch::advance();
            } else {
                if(mode=="scan_cancel_all") cache.releaseAll();
                else cache.release("adc");
                if(mode=="scan_replace"){
                    float replacement;String replacementUnit,replacementError;
                    CHECK(cache.readChannel("adc","ADS1115",0,scan,"voltage_mux7",replacement,replacementUnit,replacementError,1000));
                    CHECK(replacement>2.0f);
                }
            }
        };
        adsTestDelayHook=runScanHook;
        const bool ok=read("voltage_mux4");
        CHECK(ran && ok==(mode=="scan_parallel" || mode=="scan_lease"));
        if(!ok) CHECK(std::isnan(value) && (error=="SENSOR_SCAN_CANCELLED" || error=="SENSOR_CONFIG_CHANGED"));
        if(mode=="scan_replace"){
            SensorReading snapshot;bool stale=true;
            CHECK(cache.getSnapshot("adc",snapshot,stale,error) && snapshot.success && !stale);
        }
    } else if(mode=="scan_outer_timeout_after_success" || mode=="scan_lock_timeout_after_success") {
        SensorSampleMetadata previous, recovered;
        CHECK(read("voltage_mux4", &previous) && previous.sampleGroup);
        SensorReading snapshot; bool stale = true; String snapshotError;
        CHECK(cache.getSnapshot("adc", snapshot, stale, snapshotError) && !stale);
        CHECK(snapshot.success && snapshot.channelMetadata && snapshot.channelCount == 4);
        sensorTestNow += 1000;
        bool ran = false;
        scanHook = [&] {
            adsTestDelayHook = nullptr; ran = true;
            const unsigned transfers = Wire.transmissions;
            CHECK(cache.getSnapshot("adc", snapshot, stale, snapshotError));
            CHECK(stale && !snapshot.success && !snapshot.channelMetadata && snapshot.channelCount == 0);
            CHECK(snapshot.timestamp == 0 && snapshotError == "SENSOR_DRIVER_BUSY");
            for (float prior : snapshot.values) CHECK(std::isnan(prior));
            CHECK(Wire.transmissions == transfers);
            if (mode == "scan_lock_timeout_after_success") adsTestDenyCacheLock = true;
            else sensorTestNow += 264; // The cache deadline expires before the next driver step.
        };
        adsTestDelayHook = runScanHook;
        CHECK(!read("voltage_mux4") && ran && error == "SENSOR_SCAN_TIMEOUT" && std::isnan(value));
        adsTestDenyCacheLock = false;
        const unsigned transfers = Wire.transmissions;
        CHECK(cache.getSnapshot("adc", snapshot, stale, snapshotError));
        CHECK(stale && !snapshot.success && !snapshot.channelMetadata && snapshot.channelCount == 0);
        CHECK(snapshot.timestamp == 0);
        for (float prior : snapshot.values) CHECK(std::isnan(prior));
        CHECK(snapshotError == (mode == "scan_lock_timeout_after_success"
            ? "SENSOR_DRIVER_BUSY" : "SENSOR_SCAN_TIMEOUT"));
        CHECK(Wire.transmissions == transfers); // Passive snapshots do not reclaim or perform I/O.
        if (mode == "scan_lock_timeout_after_success") {
            CHECK(!read("voltage_mux7") && error == "SENSOR_SCAN_TIMEOUT");
            CHECK(Wire.transmissions == transfers); // Explicit read reclaims the abandoned operation.
        }
        sensorTestNow += 1000;
        CHECK(read("voltage_mux7", &recovered) && recovered.sampleGroup > previous.sampleGroup);
        CHECK(cache.getSnapshot("adc", snapshot, stale, snapshotError) && !stale && snapshot.success);
        CHECK(snapshot.channelMetadata && snapshot.channelCount == 4);
    } else if(mode=="scan_lock_timeout") {
        unsigned writes=0;
        scanHook=[&]{adsTestDelayHook=nullptr;writes=Wire.transmissions;adsTestDenyCacheLock=true;};
        adsTestDelayHook=runScanHook;
        CHECK(!read("voltage_mux4") && error=="SENSOR_SCAN_TIMEOUT" && Wire.transmissions==writes);
        adsTestDenyCacheLock=false;
        CHECK(!read("voltage_mux4") && error=="SENSOR_SCAN_TIMEOUT" && Wire.transmissions==writes);
        sensorTestNow += 1000; // Preserve the shared retry cooldown after an abandoned scan.
        CHECK(read("voltage_mux4") && Wire.starts==5);
    } else if(mode=="scan_capacity") {
        CHECK(DriverRegistry::getInstance().registerDriver("ScanPending",makeScanPending));
        unsigned depth=0;
        scanHook=[&] {
            const unsigned index=++depth;
            float nested;String nestedUnit,nestedError;
            const String id=String("pending")+String(index);
            if(index<6){
                CHECK(!cache.readChannel(id,"ScanPending",0,"{}","value",nested,nestedUnit,nestedError,1000));
                CHECK(nestedError=="SENSOR_SCAN_CANCELLED");
            } else {
                CHECK(cache.activeCount()==6);
                CHECK(!cache.readChannel(id,"ScanPending",0,"{}","value",nested,nestedUnit,nestedError,1000));
                CHECK(nestedError=="SENSOR_DRIVER_CACHE_BUSY");
                adsTestDelayHook=nullptr;
                cache.releaseAll();
            }
        };
        adsTestDelayHook=runScanHook;
        CHECK(!read("voltage_mux4") && error=="SENSOR_SCAN_CANCELLED");
        CHECK(depth==6 && cache.activeCount()==0);
    } else if(mode=="scan_timeout") {
        Wire.stuck=true;
        CHECK(!read("voltage_mux4") && std::isnan(value));
        CHECK(sensorTestNow<=1000+264 && Wire.starts==1);
        const unsigned transfers=Wire.transmissions;
        CHECK(!read("voltage_mux7") && Wire.transmissions==transfers);
    } else CHECK(false);
    adsTestDelayHook=nullptr;
    cache.releaseAll();
    CHECK(adsTestCacheLockDepth==0);
    CHECK(I2CBusManager::instance().clientCount()==0);
}

