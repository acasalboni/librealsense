// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2022 Intel Corporation. All Rights Reserved.

#include <mutex>
#include <chrono>
#include <vector>
#include <iterator>
#include <string>

#include "device.h"
#include "context.h"
#include "image.h"
#include "metadata-parser.h"

#include "ds6-device.h"
#include "ds/ds6/ds6-private.h"
#include "ds/ds-options.h"
#include "ds/ds-timestamp.h"
#include "stream.h"
#include "environment.h"
#include "ds6-color.h"
#include "ds/ds5/ds5-nonmonochrome.h"

#include "proc/depth-formats-converter.h"
#include "proc/y8i-to-y8y8.h"
#include "proc/y12i-to-y16y16.h"
#include "proc/y16i-to-y10msby10msb.h"
#include "proc/color-formats-converter.h"

#include "hdr-config.h"
#include "../common/fw/firmware-version.h"
#include "fw-update/fw-update-unsigned.h"
#include "../third-party/json.hpp"

#ifdef HWM_OVER_XU
constexpr bool hw_mon_over_xu = true;
#else
constexpr bool hw_mon_over_xu = false;
#endif

namespace librealsense
{
    std::map<uint32_t, rs2_format> ds6_depth_fourcc_to_rs2_format = {
        {rs_fourcc('Y','U','Y','2'), RS2_FORMAT_YUYV},
        {rs_fourcc('Y','U','Y','V'), RS2_FORMAT_YUYV},
        {rs_fourcc('U','Y','V','Y'), RS2_FORMAT_UYVY},
        {rs_fourcc('G','R','E','Y'), RS2_FORMAT_Y8},
        {rs_fourcc('Y','8','I',' '), RS2_FORMAT_Y8I},
        {rs_fourcc('W','1','0',' '), RS2_FORMAT_W10},
        {rs_fourcc('Y','1','6',' '), RS2_FORMAT_Y16},
        {rs_fourcc('Y','1','2','I'), RS2_FORMAT_Y12I},
        {rs_fourcc('Y','1','6','I'), RS2_FORMAT_Y16I},
        {rs_fourcc('Z','1','6',' '), RS2_FORMAT_Z16},
        {rs_fourcc('Z','1','6','H'), RS2_FORMAT_Z16H},
        {rs_fourcc('R','G','B','2'), RS2_FORMAT_BGR8},
        {rs_fourcc('M','J','P','G'), RS2_FORMAT_MJPEG},
        {rs_fourcc('B','Y','R','2'), RS2_FORMAT_RAW16}

    };
    std::map<uint32_t, rs2_stream> ds6_depth_fourcc_to_rs2_stream = {
        {rs_fourcc('Y','U','Y','2'), RS2_STREAM_COLOR},
        {rs_fourcc('Y','U','Y','V'), RS2_STREAM_COLOR},
        {rs_fourcc('U','Y','V','Y'), RS2_STREAM_INFRARED},
        {rs_fourcc('G','R','E','Y'), RS2_STREAM_INFRARED},
        {rs_fourcc('Y','8','I',' '), RS2_STREAM_INFRARED},
        {rs_fourcc('W','1','0',' '), RS2_STREAM_INFRARED},
        {rs_fourcc('Y','1','6',' '), RS2_STREAM_INFRARED},
        {rs_fourcc('Y','1','2','I'), RS2_STREAM_INFRARED},
        {rs_fourcc('Y','1','6','I'), RS2_STREAM_INFRARED},
        {rs_fourcc('R','G','B','2'), RS2_STREAM_INFRARED},
        {rs_fourcc('Z','1','6',' '), RS2_STREAM_DEPTH},
        {rs_fourcc('Z','1','6','H'), RS2_STREAM_DEPTH},
        {rs_fourcc('B','Y','R','2'), RS2_STREAM_COLOR},
        {rs_fourcc('M','J','P','G'), RS2_STREAM_COLOR}
    };

    std::vector<uint8_t> ds6_device::send_receive_raw_data(const std::vector<uint8_t>& input)
    {
        return _hw_monitor->send(input);
    }
    
    std::vector<uint8_t> ds6_device::build_command(uint32_t opcode,
        uint32_t param1,
        uint32_t param2,
        uint32_t param3,
        uint32_t param4,
        uint8_t const * data,
        size_t dataLength) const
    {
        return _hw_monitor->build_command(opcode, param1, param2, param3, param4, data, dataLength);
    }

    void ds6_device::hardware_reset()
    {
        command cmd(ds::HWRST);
        _hw_monitor->send(cmd);
    }

    void ds6_device::enter_update_state() const
    {
        _ds_devices_common_helper->enter_update_state();
    }

    std::vector<uint8_t> ds6_device::backup_flash(update_progress_callback_ptr callback)
    {
        return _ds_devices_common_helper->backup_flash(callback);
    }

    void ds6_device::update_flash(const std::vector<uint8_t>& image, update_progress_callback_ptr callback, int update_mode)
    {
        _ds_devices_common_helper->update_flash(image, callback, update_mode);
    }

    bool ds6_device::check_fw_compatibility(const std::vector<uint8_t>& image) const
    {
        return _ds_devices_common_helper->check_fw_compatibility(image);
    }

    class ds6_depth_sensor : public synthetic_sensor, public video_sensor_interface, public depth_stereo_sensor, public roi_sensor_base
    {
    public:
        explicit ds6_depth_sensor(ds6_device* owner,
            std::shared_ptr<uvc_sensor> uvc_sensor)
            : synthetic_sensor(ds::DEPTH_STEREO, uvc_sensor, owner, ds6_depth_fourcc_to_rs2_format, 
                ds6_depth_fourcc_to_rs2_stream),
            _owner(owner),
            _depth_units(-1),
            _hdr_cfg(nullptr)
        { }

        processing_blocks get_recommended_processing_blocks() const override
        {
            return get_ds_depth_recommended_proccesing_blocks();
        };

        rs2_intrinsics get_intrinsics(const stream_profile& profile) const override
        {
            rs2_intrinsics result;

            if (ds::try_get_intrinsic_by_resolution_new(*_owner->_new_calib_table_raw,
                profile.width, profile.height, &result))
            {
                return result;
            }
            else 
            {
                return get_intrinsic_by_resolution(
                    *_owner->_coefficients_table_raw,
                    ds::calibration_table_id::coefficients_table_id,
                    profile.width, profile.height);
            }
        }

        void set_frame_metadata_modifier(on_frame_md callback) override
        {
            _metadata_modifier = callback;
            auto s = get_raw_sensor().get();
            auto uvc = As< librealsense::uvc_sensor >(s);
            if(uvc)
                uvc->set_frame_metadata_modifier(callback);
        }

        void open(const stream_profiles& requests) override
        {
            group_multiple_fw_calls(*this, [&]() {
                _depth_units = get_option(RS2_OPTION_DEPTH_UNITS).query();
                set_frame_metadata_modifier([&](frame_additional_data& data) {data.depth_units = _depth_units.load(); });

                synthetic_sensor::open(requests);

                // needed in order to restore the HDR sub-preset when streaming is turned off and on
                if (_hdr_cfg && _hdr_cfg->is_enabled())
                    get_option(RS2_OPTION_HDR_ENABLED).set(1.f);
                }); //group_multiple_fw_calls
        }

        void close() override
        {
            synthetic_sensor::close();
        }

        rs2_intrinsics get_color_intrinsics(const stream_profile& profile) const
        {
            return get_intrinsic_by_resolution(
                *_owner->_color_calib_table_raw,
                ds::calibration_table_id::rgb_calibration_id,
                profile.width, profile.height);
        }

        /*
        Infrared profiles are initialized with the following logic:
        - If device has color sensor (D415 / D435), infrared profile is chosen with Y8 format
        - If device does not have color sensor:
           * if it is a rolling shutter device (D400 / D410 / D415 / D405), infrared profile is chosen with RGB8 format
           * for other devices (D420 / D430), infrared profile is chosen with Y8 format
        */
        stream_profiles init_stream_profiles() override
        {
            auto lock = environment::get_instance().get_extrinsics_graph().lock();

            auto&& results = synthetic_sensor::init_stream_profiles();

            for (auto&& p : results)
            {
                // Register stream types
                if (p->get_stream_type() == RS2_STREAM_DEPTH)
                {
                    assign_stream(_owner->_depth_stream, p);
                }
                else if (p->get_stream_type() == RS2_STREAM_INFRARED && p->get_stream_index() < 2)
                {
                    assign_stream(_owner->_left_ir_stream, p);
                }
                else if (p->get_stream_type() == RS2_STREAM_INFRARED  && p->get_stream_index() == 2)
                {
                    assign_stream(_owner->_right_ir_stream, p);
                }
                else if (p->get_stream_type() == RS2_STREAM_COLOR)
                {
                    assign_stream(_owner->_color_stream, p);
                }
                auto&& vid_profile = dynamic_cast<video_stream_profile_interface*>(p.get());

                // used when color stream comes from depth sensor (as in D405)
                if (p->get_stream_type() == RS2_STREAM_COLOR)
                {
                    const auto&& profile = to_profile(p.get());
                    std::weak_ptr<ds6_depth_sensor> wp =
                        std::dynamic_pointer_cast<ds6_depth_sensor>(this->shared_from_this());
                    vid_profile->set_intrinsics([profile, wp]()
                        {
                            auto sp = wp.lock();
                            if (sp)
                                return sp->get_color_intrinsics(profile);
                            else
                                return rs2_intrinsics{};
                        });
                }
                // Register intrinsics
                else if (p->get_format() != RS2_FORMAT_Y16) // Y16 format indicate unrectified images, no intrinsics are available for these
                {
                    const auto&& profile = to_profile(p.get());
                    std::weak_ptr<ds6_depth_sensor> wp =
                        std::dynamic_pointer_cast<ds6_depth_sensor>(this->shared_from_this());
                    vid_profile->set_intrinsics([profile, wp]()
                    {
                        auto sp = wp.lock();
                        if (sp)
                            return sp->get_intrinsics(profile);
                        else
                            return rs2_intrinsics{};
                    });
                }
            }

            return results;
        }

        float get_depth_scale() const override
        {
            if (_depth_units < 0)
                _depth_units = get_option(RS2_OPTION_DEPTH_UNITS).query();
            return _depth_units;
        }

        void set_depth_scale(float val)
        {
            _depth_units = val;
            set_frame_metadata_modifier([&](frame_additional_data& data) {data.depth_units = _depth_units.load(); });
        }

        void init_hdr_config(const option_range& exposure_range, const option_range& gain_range)
        {
            _hdr_cfg = std::make_shared<hdr_config>(*(_owner->_hw_monitor), get_raw_sensor(),
                exposure_range, gain_range);
        }

        std::shared_ptr<hdr_config> get_hdr_config() { return _hdr_cfg; }

        float get_stereo_baseline_mm() const override { return _owner->get_stereo_baseline_mm(); }

        void create_snapshot(std::shared_ptr<depth_sensor>& snapshot) const override
        {
            snapshot = std::make_shared<depth_sensor_snapshot>(get_depth_scale());
        }

        void create_snapshot(std::shared_ptr<depth_stereo_sensor>& snapshot) const override
        {
            snapshot = std::make_shared<depth_stereo_sensor_snapshot>(get_depth_scale(), get_stereo_baseline_mm());
        }

        void enable_recording(std::function<void(const depth_sensor&)> recording_function) override
        {
            //does not change over time
        }

        void enable_recording(std::function<void(const depth_stereo_sensor&)> recording_function) override
        {
            //does not change over time
        }

        float get_preset_max_value() const override
        {
            float preset_max_value = RS2_RS400_VISUAL_PRESET_COUNT - 1;
            switch (_owner->_pid)
            {
            case ds::RS400_PID:
            case ds::RS410_PID:
            case ds::RS415_PID:
            case ds::RS465_PID:
            case ds::RS460_PID:
                preset_max_value = static_cast<float>(RS2_RS400_VISUAL_PRESET_REMOVE_IR_PATTERN);
                break;
            default:
                preset_max_value = static_cast<float>(RS2_RS400_VISUAL_PRESET_MEDIUM_DENSITY);
            }
            return preset_max_value;
        }

    protected:
        const ds6_device* _owner;
        mutable std::atomic<float> _depth_units;
        float _stereo_baseline_mm;
        std::shared_ptr<hdr_config> _hdr_cfg;
    };

    bool ds6_device::is_camera_in_advanced_mode() const
    {
        return _ds_devices_common_helper->is_camera_in_advanced_mode();
    }

    float ds6_device::get_stereo_baseline_mm() const // to be ds6 adapted
    {
        using namespace ds;
        auto table = check_calib<coefficients_table>(*_coefficients_table_raw);
        return fabs(table->baseline);
    }

    std::vector<uint8_t> ds6_device::get_raw_calibration_table(ds::calibration_table_id table_id) const // to be ds6 adapted
    {
        command cmd(ds::GETINTCAL, table_id);
        return _hw_monitor->send(cmd);
    }

    std::vector<uint8_t> ds6_device::get_new_calibration_table() const // to be ds6 adapted
    {
        if (_fw_version >= firmware_version("5.11.9.5"))
        {
            command cmd(ds::RECPARAMSGET);
            return _hw_monitor->send(cmd);
        }
        return {};
    }

    ds::d400_caps ds6_device::parse_device_capabilities() const // to be ds6 adapted
    {
        using namespace ds;
        std::array<unsigned char,HW_MONITOR_BUFFER_SIZE> gvd_buf;
        _hw_monitor->get_gvd(gvd_buf.size(), gvd_buf.data(), GVD);

        // Opaque retrieval
        d400_caps val{d400_caps::CAP_UNDEFINED};
        if (gvd_buf[active_projector])  // DepthActiveMode
            val |= d400_caps::CAP_ACTIVE_PROJECTOR;
        if (gvd_buf[rgb_sensor])                           // WithRGB
            val |= d400_caps::CAP_RGB_SENSOR;
        if (gvd_buf[imu_sensor])
        {
            val |= d400_caps::CAP_IMU_SENSOR;
            if (gvd_buf[imu_acc_chip_id] == I2C_IMU_BMI055_ID_ACC)
                val |= d400_caps::CAP_BMI_055;
            else if (gvd_buf[imu_acc_chip_id] == I2C_IMU_BMI085_ID_ACC)
                val |= d400_caps::CAP_BMI_085;
            else if (hid_bmi_055_pid.end() != hid_bmi_055_pid.find(_pid))
                val |= d400_caps::CAP_BMI_055;
            else if (hid_bmi_085_pid.end() != hid_bmi_085_pid.find(_pid))
                val |= d400_caps::CAP_BMI_085;
            else
                LOG_WARNING("The IMU sensor is undefined for PID " << std::hex << _pid << " and imu_chip_id: " << gvd_buf[imu_acc_chip_id] << std::dec);
        }
        if (0xFF != (gvd_buf[fisheye_sensor_lb] & gvd_buf[fisheye_sensor_hb]))
            val |= d400_caps::CAP_FISHEYE_SENSOR;
        if (0x1 == gvd_buf[depth_sensor_type])
            val |= d400_caps::CAP_ROLLING_SHUTTER;  // e.g. ASRC
        if (0x2 == gvd_buf[depth_sensor_type])
            val |= d400_caps::CAP_GLOBAL_SHUTTER;   // e.g. AWGC
        // Option INTER_CAM_SYNC_MODE is not enabled in D405
        if (_pid != ds::RS405_PID)
            val |= d400_caps::CAP_INTERCAM_HW_SYNC;

        return val;
    }

    std::shared_ptr<synthetic_sensor> ds6_device::create_depth_device(std::shared_ptr<context> ctx,
        const std::vector<platform::uvc_device_info>& all_device_infos)
    {
        using namespace ds;

        auto&& backend = ctx->get_backend();

        std::vector<std::shared_ptr<platform::uvc_device>> depth_devices;
        for (auto&& info : filter_by_mi(all_device_infos, 0)) // Filter just mi=0, DEPTH
            depth_devices.push_back(backend.create_uvc_device(info));

        std::unique_ptr<frame_timestamp_reader> timestamp_reader_backup(new ds_timestamp_reader(backend.create_time_service()));
        std::unique_ptr<frame_timestamp_reader> timestamp_reader_metadata(new ds_timestamp_reader_from_metadata(std::move(timestamp_reader_backup)));
        auto enable_global_time_option = std::shared_ptr<global_time_option>(new global_time_option());
        auto raw_depth_ep = std::make_shared<uvc_sensor>("Raw Depth Sensor", std::make_shared<platform::multi_pins_uvc_device>(depth_devices),
            std::unique_ptr<frame_timestamp_reader>(new global_timestamp_reader(std::move(timestamp_reader_metadata), _tf_keeper, enable_global_time_option)), this);

        raw_depth_ep->register_xu(depth_xu); // make sure the XU is initialized every time we power the camera

        auto depth_ep = std::make_shared<ds6_depth_sensor>(this, raw_depth_ep);

        depth_ep->register_info(RS2_CAMERA_INFO_PHYSICAL_PORT, filter_by_mi(all_device_infos, 0).front().device_path);

        depth_ep->register_option(RS2_OPTION_GLOBAL_TIME_ENABLED, enable_global_time_option);

        depth_ep->register_processing_block(processing_block_factory::create_id_pbf(RS2_FORMAT_Y8, RS2_STREAM_INFRARED, 1));
        depth_ep->register_processing_block(processing_block_factory::create_id_pbf(RS2_FORMAT_Z16, RS2_STREAM_DEPTH));

        depth_ep->register_processing_block({ {RS2_FORMAT_W10} }, { {RS2_FORMAT_RAW10, RS2_STREAM_INFRARED, 1} }, []() { return std::make_shared<w10_converter>(RS2_FORMAT_RAW10); });
        depth_ep->register_processing_block({ {RS2_FORMAT_W10} }, { {RS2_FORMAT_Y10BPACK, RS2_STREAM_INFRARED, 1} }, []() { return std::make_shared<w10_converter>(RS2_FORMAT_Y10BPACK); });

        return depth_ep;
    }

    ds6_device::ds6_device(std::shared_ptr<context> ctx,
        const platform::backend_device_group& group)
        : device(ctx, group), global_time_interface(),
          auto_calibrated(_hw_monitor),
          _device_capabilities(ds::d400_caps::CAP_UNDEFINED),
          _depth_stream(new stream(RS2_STREAM_DEPTH)),
          _left_ir_stream(new stream(RS2_STREAM_INFRARED, 1)),
          _right_ir_stream(new stream(RS2_STREAM_INFRARED, 2)),
          _color_stream(nullptr)
    {
        _depth_device_idx = add_sensor(create_depth_device(ctx, group.uvc_devices));
        init(ctx, group);
    }

    void ds6_device::init(std::shared_ptr<context> ctx,
        const platform::backend_device_group& group)
    {
        using namespace ds;

        auto&& backend = ctx->get_backend();
        auto& raw_sensor = get_raw_depth_sensor();
        _pid = group.uvc_devices.front().pid;

        _color_calib_table_raw = [this]()
        {
            return get_raw_calibration_table(rgb_calibration_id);
        };

        if (((hw_mon_over_xu) && (RS400_IMU_PID != _pid)) || (!group.usb_devices.size()))
        {
            _hw_monitor = std::make_shared<hw_monitor>(
                std::make_shared<locked_transfer>(
                    std::make_shared<command_transfer_over_xu>(
                        raw_sensor, depth_xu, DS5_HWMONITOR),
                    raw_sensor));
        }
        else
        {
            _hw_monitor = std::make_shared<hw_monitor>(
                std::make_shared<locked_transfer>(
                    backend.create_usb_device(group.usb_devices.front()), raw_sensor));
        }

        _ds_devices_common_helper = std::make_shared<ds_devices_common>(
            this, ds6, _hw_monitor);

        // Define Left-to-Right extrinsics calculation (lazy)
        // Reference CS - Right-handed; positive [X,Y,Z] point to [Left,Up,Forward] accordingly.
        _left_right_extrinsics = std::make_shared<lazy<rs2_extrinsics>>([this]()
            {
                rs2_extrinsics ext = identity_matrix();
                auto table = check_calib<coefficients_table>(*_coefficients_table_raw);
                ext.translation[0] = 0.001f * table->baseline; // mm to meters
                return ext;
            });

        environment::get_instance().get_extrinsics_graph().register_same_extrinsics(*_depth_stream, *_left_ir_stream);
        environment::get_instance().get_extrinsics_graph().register_extrinsics(*_depth_stream, *_right_ir_stream, _left_right_extrinsics);

        register_stream_to_extrinsic_group(*_depth_stream, 0);
        register_stream_to_extrinsic_group(*_left_ir_stream, 0);
        register_stream_to_extrinsic_group(*_right_ir_stream, 0);

        _coefficients_table_raw = [this]() { return get_raw_calibration_table(coefficients_table_id); };
        _new_calib_table_raw = [this]() { return get_new_calibration_table(); };

        std::string device_name = (rs400_sku_names.end() != rs400_sku_names.find(_pid)) ? rs400_sku_names.at(_pid) : "RS4xx";

        std::vector<uint8_t> gvd_buff(HW_MONITOR_BUFFER_SIZE);

        auto& depth_sensor = get_depth_sensor();
        auto& raw_depth_sensor = get_raw_depth_sensor();

        using namespace platform;

        // minimal firmware version in which hdr feature is supported
        firmware_version hdr_firmware_version("5.12.8.100");

        std::string optic_serial, asic_serial, pid_hex_str, usb_type_str;
        bool advanced_mode, usb_modality;
        group_multiple_fw_calls(depth_sensor, [&]() {

            _hw_monitor->get_gvd(gvd_buff.size(), gvd_buff.data(), GVD);

            optic_serial = _hw_monitor->get_module_serial_string(gvd_buff, module_serial_offset);
            asic_serial = _hw_monitor->get_module_serial_string(gvd_buff, module_asic_serial_offset);
            auto fwv = _hw_monitor->get_firmware_version_string(gvd_buff, camera_fw_version_offset);
            _fw_version = firmware_version(fwv);

            _recommended_fw_version = firmware_version(D4XX_RECOMMENDED_FIRMWARE_VERSION);
            if (_fw_version >= firmware_version("5.10.4.0"))
                _device_capabilities = parse_device_capabilities();

            advanced_mode = is_camera_in_advanced_mode();

            auto _usb_mode = usb3_type;
            usb_type_str = usb_spec_names.at(_usb_mode);
            usb_modality = (_fw_version >= firmware_version("5.9.8.0"));
            if (usb_modality)
            {
                _usb_mode = raw_depth_sensor.get_usb_specification();
                if (usb_spec_names.count(_usb_mode) && (usb_undefined != _usb_mode))
                    usb_type_str = usb_spec_names.at(_usb_mode);
                else  // Backend fails to provide USB descriptor  - occurs with RS3 build. Requires further work
                    usb_modality = false;
            }

            if (_fw_version >= firmware_version("5.12.1.1"))
            {
                depth_sensor.register_processing_block(processing_block_factory::create_id_pbf(RS2_FORMAT_Z16H, RS2_STREAM_DEPTH));
            }

            depth_sensor.register_processing_block(
                { {RS2_FORMAT_Y8I} },
                { {RS2_FORMAT_Y8, RS2_STREAM_INFRARED, 1} , {RS2_FORMAT_Y8, RS2_STREAM_INFRARED, 2} },
                []() { return std::make_shared<y8i_to_y8y8>(); }
            ); // L+R

            if (_pid == RS_D585_PID || _pid == RS_S585_PID)
            {
                depth_sensor.register_processing_block(
                    { RS2_FORMAT_Y16I },
                    { {RS2_FORMAT_Y16, RS2_STREAM_INFRARED, 1}, {RS2_FORMAT_Y16, RS2_STREAM_INFRARED, 2} },
                    []() {return std::make_shared<y16i_to_y10msby10msb>(); }
                );
            }
            else
            {
                depth_sensor.register_processing_block(
                    { RS2_FORMAT_Y12I },
                    { {RS2_FORMAT_Y16, RS2_STREAM_INFRARED, 1}, {RS2_FORMAT_Y16, RS2_STREAM_INFRARED, 2} },
                    []() {return std::make_shared<y12i_to_y16y16>(); }
                );
            }
                
            pid_hex_str = hexify(_pid);

            if ((_pid == RS416_PID || _pid == RS416_RGB_PID) && _fw_version >= firmware_version("5.12.0.1"))
            {
                depth_sensor.register_option(RS2_OPTION_HARDWARE_PRESET,
                    std::make_shared<uvc_xu_option<uint8_t>>(raw_depth_sensor, depth_xu, DS5_HARDWARE_PRESET,
                        "Hardware pipe configuration"));
                depth_sensor.register_option(RS2_OPTION_LED_POWER,
                    std::make_shared<uvc_xu_option<uint16_t>>(raw_depth_sensor, depth_xu, DS5_LED_PWR,
                        "Set the power level of the LED, with 0 meaning LED off"));
            }

            if (_fw_version >= firmware_version("5.6.3.0"))
            {
                _is_locked = _ds_devices_common_helper->is_locked(GVD, is_camera_locked_offset);
            }

            if (_fw_version >= firmware_version("5.5.8.0"))
            {
                depth_sensor.register_option(RS2_OPTION_OUTPUT_TRIGGER_ENABLED,
                    std::make_shared<uvc_xu_option<uint8_t>>(raw_depth_sensor, depth_xu, DS5_EXT_TRIGGER,
                        "Generate trigger from the camera to external device once per frame"));

                auto error_control = std::make_shared<uvc_xu_option<uint8_t>>(raw_depth_sensor, depth_xu, DS5_ERROR_REPORTING, "Error reporting");

                _polling_error_handler = std::make_shared<polling_error_handler>(1000,
                    error_control,
                    raw_depth_sensor.get_notifications_processor(),
                    std::make_shared<ds_notification_decoder>());

                depth_sensor.register_option(RS2_OPTION_ERROR_POLLING_ENABLED, std::make_shared<polling_errors_disable>(_polling_error_handler));

                depth_sensor.register_option(RS2_OPTION_ASIC_TEMPERATURE,
                    std::make_shared<asic_and_projector_temperature_options>(raw_depth_sensor,
                        RS2_OPTION_ASIC_TEMPERATURE));
            }

            std::shared_ptr<option> exposure_option = nullptr;
            std::shared_ptr<option> gain_option = nullptr;
            std::shared_ptr<hdr_option> hdr_enabled_option = nullptr;

            //EXPOSURE AND GAIN - preparing uvc options
            auto uvc_xu_exposure_option = std::make_shared<uvc_xu_option<uint32_t>>(raw_depth_sensor,
                depth_xu,
                DS5_EXPOSURE,
                "Depth Exposure (usec)");
            option_range exposure_range = uvc_xu_exposure_option->get_range();
            auto uvc_pu_gain_option = std::make_shared<uvc_pu_option>(raw_depth_sensor, RS2_OPTION_GAIN);
            option_range gain_range = uvc_pu_gain_option->get_range();

            //AUTO EXPOSURE
            auto enable_auto_exposure = std::make_shared<uvc_xu_option<uint8_t>>(raw_depth_sensor,
                depth_xu,
                DS5_ENABLE_AUTO_EXPOSURE,
                "Enable Auto Exposure");
            depth_sensor.register_option(RS2_OPTION_ENABLE_AUTO_EXPOSURE, enable_auto_exposure);

            // register HDR options
            //auto global_shutter_mask = d400_caps::CAP_GLOBAL_SHUTTER;
            if ((_fw_version >= hdr_firmware_version))// && ((_device_capabilities & global_shutter_mask) == global_shutter_mask) )
            {
                auto ds6_depth = As<ds6_depth_sensor, synthetic_sensor>(&get_depth_sensor());
                ds6_depth->init_hdr_config(exposure_range, gain_range);
                auto&& hdr_cfg = ds6_depth->get_hdr_config();

                // values from 4 to 14 - for internal use
                // value 15 - saved for emiter on off subpreset
                option_range hdr_id_range = { 0.f /*min*/, 3.f /*max*/, 1.f /*step*/, 1.f /*default*/ };
                auto hdr_id_option = std::make_shared<hdr_option>(hdr_cfg, RS2_OPTION_SEQUENCE_NAME, hdr_id_range,
                    std::map<float, std::string>{ {0.f, "0"}, { 1.f, "1" }, { 2.f, "2" }, { 3.f, "3" } });
                depth_sensor.register_option(RS2_OPTION_SEQUENCE_NAME, hdr_id_option);

                option_range hdr_sequence_size_range = { 2.f /*min*/, 2.f /*max*/, 1.f /*step*/, 2.f /*default*/ };
                auto hdr_sequence_size_option = std::make_shared<hdr_option>(hdr_cfg, RS2_OPTION_SEQUENCE_SIZE, hdr_sequence_size_range,
                    std::map<float, std::string>{ { 2.f, "2" } });
                depth_sensor.register_option(RS2_OPTION_SEQUENCE_SIZE, hdr_sequence_size_option);

                option_range hdr_sequ_id_range = { 0.f /*min*/, 2.f /*max*/, 1.f /*step*/, 0.f /*default*/ };
                auto hdr_sequ_id_option = std::make_shared<hdr_option>(hdr_cfg, RS2_OPTION_SEQUENCE_ID, hdr_sequ_id_range,
                    std::map<float, std::string>{ {0.f, "UVC"}, { 1.f, "1" }, { 2.f, "2" } });
                depth_sensor.register_option(RS2_OPTION_SEQUENCE_ID, hdr_sequ_id_option);

                option_range hdr_enable_range = { 0.f /*min*/, 1.f /*max*/, 1.f /*step*/, 0.f /*default*/ };
                hdr_enabled_option = std::make_shared<hdr_option>(hdr_cfg, RS2_OPTION_HDR_ENABLED, hdr_enable_range);
                depth_sensor.register_option(RS2_OPTION_HDR_ENABLED, hdr_enabled_option);

                //EXPOSURE AND GAIN - preparing hdr options
                auto hdr_exposure_option = std::make_shared<hdr_option>(hdr_cfg, RS2_OPTION_EXPOSURE, exposure_range);
                auto hdr_gain_option = std::make_shared<hdr_option>(hdr_cfg, RS2_OPTION_GAIN, gain_range);

                //EXPOSURE AND GAIN - preparing hybrid options
                auto hdr_conditional_exposure_option = std::make_shared<hdr_conditional_option>(hdr_cfg, uvc_xu_exposure_option, hdr_exposure_option);
                auto hdr_conditional_gain_option = std::make_shared<hdr_conditional_option>(hdr_cfg, uvc_pu_gain_option, hdr_gain_option);

                exposure_option = hdr_conditional_exposure_option;
                gain_option = hdr_conditional_gain_option;

                std::vector<std::pair<std::shared_ptr<option>, std::string>> options_and_reasons = { std::make_pair(hdr_enabled_option,
                        "Auto Exposure cannot be set while HDR is enabled") };
                depth_sensor.register_option(RS2_OPTION_ENABLE_AUTO_EXPOSURE,
                    std::make_shared<gated_option>(
                        enable_auto_exposure,
                        options_and_reasons));
            }
            else
            {
                exposure_option = uvc_xu_exposure_option;
                gain_option = uvc_pu_gain_option;
            }

            //EXPOSURE
            depth_sensor.register_option(RS2_OPTION_EXPOSURE,
                std::make_shared<auto_disabling_control>(
                    exposure_option,
                    enable_auto_exposure));

            //GAIN
            depth_sensor.register_option(RS2_OPTION_GAIN,
                std::make_shared<auto_disabling_control>(
                    gain_option,
                    enable_auto_exposure));

            // Alternating laser pattern is applicable for global shutter/active SKUs
            auto mask = d400_caps::CAP_GLOBAL_SHUTTER | d400_caps::CAP_ACTIVE_PROJECTOR;
            // Alternating laser pattern should be set and query in a different way according to the firmware version
            if ((_fw_version >= firmware_version("5.11.3.0")) && ((_device_capabilities & mask) == mask))
            {
                bool is_fw_version_using_id = (_fw_version >= firmware_version("5.12.8.100"));
                auto alternating_emitter_opt = std::make_shared<alternating_emitter_option>(*_hw_monitor, &raw_depth_sensor, is_fw_version_using_id);
                auto emitter_always_on_opt = std::make_shared<emitter_always_on_option>(*_hw_monitor, &depth_sensor);

                if ((_fw_version >= firmware_version("5.12.1.0")) && ((_device_capabilities & d400_caps::CAP_GLOBAL_SHUTTER) == d400_caps::CAP_GLOBAL_SHUTTER))
                {
                    std::vector<std::pair<std::shared_ptr<option>, std::string>> options_and_reasons = { std::make_pair(alternating_emitter_opt,
                        "Emitter always ON cannot be set while Emitter ON/OFF is enabled") };
                    depth_sensor.register_option(RS2_OPTION_EMITTER_ALWAYS_ON,
                        std::make_shared<gated_option>(
                            emitter_always_on_opt,
                            options_and_reasons));
                }

                if (_fw_version >= hdr_firmware_version)
                {
                    std::vector<std::pair<std::shared_ptr<option>, std::string>> options_and_reasons = { std::make_pair(hdr_enabled_option, "Emitter ON/OFF cannot be set while HDR is enabled"),
                            std::make_pair(emitter_always_on_opt, "Emitter ON/OFF cannot be set while Emitter always ON is enabled") };
                    depth_sensor.register_option(RS2_OPTION_EMITTER_ON_OFF,
                        std::make_shared<gated_option>(
                            alternating_emitter_opt,
                            options_and_reasons
                            ));
                }
                else if ((_fw_version >= firmware_version("5.12.1.0")) && ((_device_capabilities & d400_caps::CAP_GLOBAL_SHUTTER) == d400_caps::CAP_GLOBAL_SHUTTER))
                {
                    std::vector<std::pair<std::shared_ptr<option>, std::string>> options_and_reasons = { std::make_pair(emitter_always_on_opt,
                        "Emitter ON/OFF cannot be set while Emitter always ON is enabled") };
                    depth_sensor.register_option(RS2_OPTION_EMITTER_ON_OFF,
                        std::make_shared<gated_option>(
                            alternating_emitter_opt,
                            options_and_reasons));
                }
                else
                {
                    depth_sensor.register_option(RS2_OPTION_EMITTER_ON_OFF, alternating_emitter_opt);
                }
            }
            else if (_fw_version >= firmware_version("5.10.9.0") && 
                (_device_capabilities & d400_caps::CAP_ACTIVE_PROJECTOR) == d400_caps::CAP_ACTIVE_PROJECTOR &&
                _fw_version.experimental()) // Not yet available in production firmware
            {
                depth_sensor.register_option(RS2_OPTION_EMITTER_ON_OFF, std::make_shared<emitter_on_and_off_option>(*_hw_monitor, &raw_depth_sensor));
            }

            if ((_device_capabilities & d400_caps::CAP_INTERCAM_HW_SYNC) == d400_caps::CAP_INTERCAM_HW_SYNC)
            {
                if (_fw_version >= firmware_version("5.12.12.100") && (_device_capabilities & d400_caps::CAP_GLOBAL_SHUTTER) == d400_caps::CAP_GLOBAL_SHUTTER)
                {
                    depth_sensor.register_option(RS2_OPTION_INTER_CAM_SYNC_MODE,
                        std::make_shared<external_sync_mode>(*_hw_monitor, &raw_depth_sensor, 3));
                }
                else if (_fw_version >= firmware_version("5.12.4.0") && (_device_capabilities & d400_caps::CAP_GLOBAL_SHUTTER) == d400_caps::CAP_GLOBAL_SHUTTER)
                {
                    depth_sensor.register_option(RS2_OPTION_INTER_CAM_SYNC_MODE,
                        std::make_shared<external_sync_mode>(*_hw_monitor, &raw_depth_sensor, 2));
                }
                else if (_fw_version >= firmware_version("5.9.15.1"))
                {
                    depth_sensor.register_option(RS2_OPTION_INTER_CAM_SYNC_MODE,
                        std::make_shared<external_sync_mode>(*_hw_monitor, &raw_depth_sensor, 1));
                }
            }

            roi_sensor_interface* roi_sensor = dynamic_cast<roi_sensor_interface*>(&depth_sensor);
            if (roi_sensor)
                roi_sensor->set_roi_method(std::make_shared<ds5_auto_exposure_roi_method>(*_hw_monitor));

            depth_sensor.register_option(RS2_OPTION_STEREO_BASELINE, std::make_shared<const_value_option>("Distance in mm between the stereo imagers",
                lazy<float>([this]() { return get_stereo_baseline_mm(); })));

            if (advanced_mode && _fw_version >= firmware_version("5.6.3.0"))
            {
                auto depth_scale = std::make_shared<depth_scale_option>(*_hw_monitor);
                auto depth_sensor = As<ds6_depth_sensor, synthetic_sensor>(&get_depth_sensor());
                assert(depth_sensor);

                depth_scale->add_observer([depth_sensor](float val)
                {
                    depth_sensor->set_depth_scale(val);
                });

                depth_sensor->register_option(RS2_OPTION_DEPTH_UNITS, depth_scale);
            }
            else
            {
                float default_depth_units = 0.001f; //meters
                // default depth units is different for D405
                if (_pid == RS405_PID)
                    default_depth_units = 0.0001f;  //meters
                depth_sensor.register_option(RS2_OPTION_DEPTH_UNITS, std::make_shared<const_value_option>("Number of meters represented by a single depth unit",
                    lazy<float>([default_depth_units]()
                        { return default_depth_units; })));
            }
            
            // Metadata registration
            depth_sensor.register_metadata(RS2_FRAME_METADATA_FRAME_TIMESTAMP, make_uvc_header_parser(&uvc_header::timestamp));
        }); //group_multiple_fw_calls

        // attributes of md_capture_timing
        auto md_prop_offset = offsetof(metadata_raw, mode) +
            offsetof(md_depth_mode, depth_y_mode) +
            offsetof(md_depth_y_normal_mode, intel_capture_timing);

        depth_sensor.register_metadata(RS2_FRAME_METADATA_FRAME_COUNTER, make_attribute_parser(&md_capture_timing::frame_counter, md_capture_timing_attributes::frame_counter_attribute, md_prop_offset));
        depth_sensor.register_metadata(RS2_FRAME_METADATA_SENSOR_TIMESTAMP, make_rs400_sensor_ts_parser(make_uvc_header_parser(&uvc_header::timestamp),
            make_attribute_parser(&md_capture_timing::sensor_timestamp, md_capture_timing_attributes::sensor_timestamp_attribute, md_prop_offset)));

        // attributes of md_capture_stats
        md_prop_offset = offsetof(metadata_raw, mode) +
            offsetof(md_depth_mode, depth_y_mode) +
            offsetof(md_depth_y_normal_mode, intel_capture_stats);

        depth_sensor.register_metadata(RS2_FRAME_METADATA_WHITE_BALANCE, make_attribute_parser(&md_capture_stats::white_balance, md_capture_stat_attributes::white_balance_attribute, md_prop_offset));

        // attributes of md_depth_control
        md_prop_offset = offsetof(metadata_raw, mode) +
            offsetof(md_depth_mode, depth_y_mode) +
            offsetof(md_depth_y_normal_mode, intel_depth_control);

        depth_sensor.register_metadata(RS2_FRAME_METADATA_GAIN_LEVEL, make_attribute_parser(&md_depth_control::manual_gain, md_depth_control_attributes::gain_attribute, md_prop_offset));
        depth_sensor.register_metadata(RS2_FRAME_METADATA_ACTUAL_EXPOSURE, make_attribute_parser(&md_depth_control::manual_exposure, md_depth_control_attributes::exposure_attribute, md_prop_offset));
        depth_sensor.register_metadata(RS2_FRAME_METADATA_AUTO_EXPOSURE, make_attribute_parser(&md_depth_control::auto_exposure_mode, md_depth_control_attributes::ae_mode_attribute, md_prop_offset));

        depth_sensor.register_metadata(RS2_FRAME_METADATA_FRAME_LASER_POWER, make_attribute_parser(&md_depth_control::laser_power, md_depth_control_attributes::laser_pwr_attribute, md_prop_offset));
        depth_sensor.register_metadata(RS2_FRAME_METADATA_FRAME_LASER_POWER_MODE, make_attribute_parser(&md_depth_control::emitterMode, md_depth_control_attributes::emitter_mode_attribute, md_prop_offset,
            [](const rs2_metadata_type& param) { return param == 1 ? 1 : 0; })); // starting at version 2.30.1 this control is superceeded by RS2_FRAME_METADATA_FRAME_EMITTER_MODE
        depth_sensor.register_metadata(RS2_FRAME_METADATA_EXPOSURE_PRIORITY, make_attribute_parser(&md_depth_control::exposure_priority, md_depth_control_attributes::exposure_priority_attribute, md_prop_offset));
        depth_sensor.register_metadata(RS2_FRAME_METADATA_EXPOSURE_ROI_LEFT, make_attribute_parser(&md_depth_control::exposure_roi_left, md_depth_control_attributes::roi_attribute, md_prop_offset));
        depth_sensor.register_metadata(RS2_FRAME_METADATA_EXPOSURE_ROI_RIGHT, make_attribute_parser(&md_depth_control::exposure_roi_right, md_depth_control_attributes::roi_attribute, md_prop_offset));
        depth_sensor.register_metadata(RS2_FRAME_METADATA_EXPOSURE_ROI_TOP, make_attribute_parser(&md_depth_control::exposure_roi_top, md_depth_control_attributes::roi_attribute, md_prop_offset));
        depth_sensor.register_metadata(RS2_FRAME_METADATA_EXPOSURE_ROI_BOTTOM, make_attribute_parser(&md_depth_control::exposure_roi_bottom, md_depth_control_attributes::roi_attribute, md_prop_offset));
        depth_sensor.register_metadata(RS2_FRAME_METADATA_FRAME_EMITTER_MODE, make_attribute_parser(&md_depth_control::emitterMode, md_depth_control_attributes::emitter_mode_attribute, md_prop_offset));
        depth_sensor.register_metadata(RS2_FRAME_METADATA_FRAME_LED_POWER, make_attribute_parser(&md_depth_control::ledPower, md_depth_control_attributes::led_power_attribute, md_prop_offset));

        // md_configuration - will be used for internal validation only
        md_prop_offset = offsetof(metadata_raw, mode) + offsetof(md_depth_mode, depth_y_mode) + offsetof(md_depth_y_normal_mode, intel_configuration);

        depth_sensor.register_metadata((rs2_frame_metadata_value)RS2_FRAME_METADATA_HW_TYPE, make_attribute_parser(&md_configuration::hw_type, md_configuration_attributes::hw_type_attribute, md_prop_offset));
        depth_sensor.register_metadata((rs2_frame_metadata_value)RS2_FRAME_METADATA_SKU_ID, make_attribute_parser(&md_configuration::sku_id, md_configuration_attributes::sku_id_attribute, md_prop_offset));
        depth_sensor.register_metadata((rs2_frame_metadata_value)RS2_FRAME_METADATA_FORMAT, make_attribute_parser(&md_configuration::format, md_configuration_attributes::format_attribute, md_prop_offset));
        depth_sensor.register_metadata((rs2_frame_metadata_value)RS2_FRAME_METADATA_WIDTH, make_attribute_parser(&md_configuration::width, md_configuration_attributes::width_attribute, md_prop_offset));
        depth_sensor.register_metadata((rs2_frame_metadata_value)RS2_FRAME_METADATA_HEIGHT, make_attribute_parser(&md_configuration::height, md_configuration_attributes::height_attribute, md_prop_offset));
        depth_sensor.register_metadata((rs2_frame_metadata_value)RS2_FRAME_METADATA_ACTUAL_FPS,  std::make_shared<ds5_md_attribute_actual_fps> ());

        if (_fw_version >= firmware_version("5.12.7.0"))
        {
            depth_sensor.register_metadata(RS2_FRAME_METADATA_GPIO_INPUT_DATA, make_attribute_parser(&md_configuration::gpioInputData, md_configuration_attributes::gpio_input_data_attribute, md_prop_offset));
        }

        if (_fw_version >= hdr_firmware_version)
        {
            // attributes of md_capture_timing
            auto md_prop_offset = offsetof(metadata_raw, mode) + offsetof(md_depth_mode, depth_y_mode) + offsetof(md_depth_y_normal_mode, intel_configuration);

            depth_sensor.register_metadata(RS2_FRAME_METADATA_SEQUENCE_SIZE,
                make_attribute_parser(&md_configuration::sub_preset_info,
                    md_configuration_attributes::sub_preset_info_attribute, md_prop_offset ,
                [](const rs2_metadata_type& param) {
                        // bit mask and offset used to get data from bitfield
                        return (param & md_configuration::SUB_PRESET_BIT_MASK_SEQUENCE_SIZE)
                            >> md_configuration::SUB_PRESET_BIT_OFFSET_SEQUENCE_SIZE;
                    }));

            depth_sensor.register_metadata(RS2_FRAME_METADATA_SEQUENCE_ID,
                make_attribute_parser(&md_configuration::sub_preset_info,
                    md_configuration_attributes::sub_preset_info_attribute, md_prop_offset ,
                [](const rs2_metadata_type& param) {
                        // bit mask and offset used to get data from bitfield
                        return (param & md_configuration::SUB_PRESET_BIT_MASK_SEQUENCE_ID)
                            >> md_configuration::SUB_PRESET_BIT_OFFSET_SEQUENCE_ID;
                    }));

            depth_sensor.register_metadata(RS2_FRAME_METADATA_SEQUENCE_NAME,
                make_attribute_parser(&md_configuration::sub_preset_info,
                    md_configuration_attributes::sub_preset_info_attribute, md_prop_offset,
                    [](const rs2_metadata_type& param) {
                        // bit mask and offset used to get data from bitfield
                        return (param & md_configuration::SUB_PRESET_BIT_MASK_ID)
                            >> md_configuration::SUB_PRESET_BIT_OFFSET_ID;
                    }));
        }


        register_info(RS2_CAMERA_INFO_NAME, device_name);
        register_info(RS2_CAMERA_INFO_SERIAL_NUMBER, optic_serial);
        register_info(RS2_CAMERA_INFO_ASIC_SERIAL_NUMBER, asic_serial);
        register_info(RS2_CAMERA_INFO_FIRMWARE_UPDATE_ID, asic_serial);
        register_info(RS2_CAMERA_INFO_FIRMWARE_VERSION, _fw_version);
        register_info(RS2_CAMERA_INFO_PHYSICAL_PORT, group.uvc_devices.front().device_path);
        register_info(RS2_CAMERA_INFO_DEBUG_OP_CODE, std::to_string(static_cast<int>(fw_cmd::GLD)));
        register_info(RS2_CAMERA_INFO_ADVANCED_MODE, ((advanced_mode) ? "YES" : "NO"));
        register_info(RS2_CAMERA_INFO_PRODUCT_ID, pid_hex_str);
        register_info(RS2_CAMERA_INFO_PRODUCT_LINE, "D400");
        register_info(RS2_CAMERA_INFO_RECOMMENDED_FIRMWARE_VERSION, _recommended_fw_version);
        register_info(RS2_CAMERA_INFO_CAMERA_LOCKED, _is_locked ? "YES" : "NO");

        if (usb_modality)
            register_info(RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR, usb_type_str);

        std::string curr_version= _fw_version;
    }

    void ds6_device::create_snapshot(std::shared_ptr<debug_interface>& snapshot) const
    {
        //TODO: Implement
    }
    void ds6_device::enable_recording(std::function<void(const debug_interface&)> record_action)
    {
        //TODO: Implement
    }

    platform::usb_spec ds6_device::get_usb_spec() const
    {
        if(!supports_info(RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR))
            return platform::usb_undefined;
        auto str = get_info(RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR);
        for (auto u : platform::usb_spec_names)
        {
            if (u.second.compare(str) == 0)
                return u.first;
        }
        return platform::usb_undefined;
    }


    double ds6_device::get_device_time_ms()
    {
        //// TODO: Refactor the following query with an extension.
        //if (dynamic_cast<const platform::playback_backend*>(&(get_context()->get_backend())) != nullptr)
        //{
        //    throw not_implemented_exception("device time not supported for backend.");
        //}

        if (!_hw_monitor)
            throw wrong_api_call_sequence_exception("_hw_monitor is not initialized yet");

        command cmd(ds::MRD, ds::REGISTER_CLOCK_0, ds::REGISTER_CLOCK_0 + 4);
        auto res = _hw_monitor->send(cmd);

        if (res.size() < sizeof(uint32_t))
        {
            LOG_DEBUG("size(res):" << res.size());
            throw std::runtime_error("Not enough bytes returned from the firmware!");
        }
        uint32_t dt = *(uint32_t*)res.data();
        double ts = dt * TIMESTAMP_USEC_TO_MSEC;
        return ts;
    }

    command ds6_device::get_firmware_logs_command() const
    {
        return command{ ds::GLD, 0x1f4 };
    }

    command ds6_device::get_flash_logs_command() const
    {
        return command{ ds::FRB, 0x17a000, 0x3f8 };
    }
}
