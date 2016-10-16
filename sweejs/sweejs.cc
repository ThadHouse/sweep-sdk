#include <stdexcept>
#include <utility>

#include "sweejs.h"

// Wrapper Object

struct SweepError final : std::runtime_error {
  using Base = std::runtime_error;

  SweepError(const char* what) : Base{what} {}
};

Sweep::Sweep() {
  ::sweep_error_s error = nullptr;
  auto devptr = ::sweep_device_construct_simple(&error);

  if (error) {
    throw SweepError{"device construction failed"};
  }

  std::shared_ptr<::sweep_device> arc{devptr, [](::sweep_device_s dev) { ::sweep_device_destruct(dev); }};
  device = std::move(arc);
}

Sweep::Sweep(const char* port, int32_t baudrate, int32_t timeout) {
  ::sweep_error_s error = nullptr;
  auto devptr = ::sweep_device_construct(port, baudrate, timeout, &error);

  if (error) {
    throw SweepError{"device construction failed"};
  }

  std::shared_ptr<::sweep_device> arc{devptr, [](::sweep_device_s dev) { ::sweep_device_destruct(dev); }};
  device = std::move(arc);
}

// NAN Bindings

NAN_MODULE_INIT(Sweep::Init) {
  const auto whoami = Nan::New("Sweep").ToLocalChecked();

  auto fnTp = Nan::New<v8::FunctionTemplate>(New);
  fnTp->SetClassName(whoami);
  fnTp->InstanceTemplate()->SetInternalFieldCount(1);

  SetPrototypeMethod(fnTp, "startScanning", startScanning);
  SetPrototypeMethod(fnTp, "stopScanning", stopScanning);
  SetPrototypeMethod(fnTp, "scan", scan);
  SetPrototypeMethod(fnTp, "getMotorSpeed", getMotorSpeed);
  SetPrototypeMethod(fnTp, "setMotorSpeed", setMotorSpeed);
  SetPrototypeMethod(fnTp, "getSampleRate", getSampleRate);
  SetPrototypeMethod(fnTp, "reset", reset);

  const auto fn = Nan::GetFunction(fnTp).ToLocalChecked();
  constructor().Reset(fn);

  Nan::Set(target, whoami, fn);
}

NAN_METHOD(Sweep::New) {
  // auto-detect or port, baud, timeout
  const auto simple = info.Length() == 0;
  const auto config = info.Length() == 3 && info[0]->IsString() && info[1]->IsNumber() && info[2]->IsNumber();

  if (!simple && !config) {
    return Nan::ThrowTypeError("No arguments for auto-detection or serial port, baudrate, timeout expected");
  }

  if (info.IsConstructCall()) {
    Sweep* self = nullptr;

    try {
      if (simple) {
        self = new Sweep();
      } else if (config) {
        const Nan::Utf8String utf8port{info[0]};

        if (!(*utf8port)) {
          return Nan::ThrowError("UTF-8 conversion error for serial port string");
        }

        const auto port = *utf8port;
        const auto baudrate = Nan::To<int32_t>(info[1]).FromJust();
        const auto timeout = Nan::To<int32_t>(info[2]).FromJust();
        self = new Sweep(port, baudrate, timeout);
      } else {
        return Nan::ThrowError("Unable to create device"); // unreachable
      }
    } catch (const SweepError& e) {
      return Nan::ThrowError(e.what());
    }

    self->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  } else {
    auto init = Nan::New(constructor());
    info.GetReturnValue().Set(init->NewInstance());
  }
}

NAN_METHOD(Sweep::startScanning) {
  auto* const self = Nan::ObjectWrap::Unwrap<Sweep>(info.Holder());

  if (info.Length() != 0) {
    return Nan::ThrowTypeError("No arguments expected");
  }

  ::sweep_error_s error = nullptr;
  ::sweep_device_start_scanning(self->device.get(), &error);

  if (error) {
    Nan::ThrowError(::sweep_error_message(error));
    ::sweep_error_destruct(error);
  }
}

NAN_METHOD(Sweep::stopScanning) {
  auto* const self = Nan::ObjectWrap::Unwrap<Sweep>(info.Holder());

  if (info.Length() != 0) {
    return Nan::ThrowTypeError("No arguments expected");
  }

  ::sweep_error_s error = nullptr;
  ::sweep_device_stop_scanning(self->device.get(), &error);

  if (error) {
    Nan::ThrowError(::sweep_error_message(error));
    ::sweep_error_destruct(error);
  }
}

NAN_METHOD(Sweep::scan) {
  auto* const self = Nan::ObjectWrap::Unwrap<Sweep>(info.Holder());

  if (info.Length() != 2 || !info[0]->IsNumber() || !info[1]->IsFunction()) {
    return Nan::ThrowTypeError("Timeout and callback expected");
  }

  const auto timeout = Nan::To<int32_t>(info[0]).FromJust();
  const auto function = info[1].As<v8::Function>();

  struct AsyncScanWorker final : Nan::AsyncWorker {
    AsyncScanWorker(Sweep& sweep, Nan::Callback* callback, int32_t timeout)
        : Nan::AsyncWorker(callback), sweep{sweep}, timeout{timeout} {}

    void Execute() override {
      ::sweep_error_s error = nullptr;
      scan = ::sweep_device_get_scan(sweep.device.get(), timeout, &error);

      if (error) {
        SetErrorMessage(::sweep_error_message(error));
        ::sweep_error_destruct(error);
      }
    }

    void HandleOKCallback() override {
      Nan::HandleScope scope;

      auto n = ::sweep_scan_get_number_of_samples(scan);

      auto samples = Nan::New<v8::Array>(n);

      for (int32_t i = 0; i < n; ++i) {
        const auto angle = Nan::New<v8::Number>(::sweep_scan_get_angle(scan, i));
        const auto distance = Nan::New<v8::Number>(::sweep_scan_get_distance(scan, i));

        const auto anglekey = Nan::New<v8::String>("angle").ToLocalChecked();
        const auto distancekey = Nan::New<v8::String>("distance").ToLocalChecked();

        // sample = {'angle': 360, 'distance': 20}
        const auto sample = Nan::New<v8::Object>();
        Nan::Set(sample, anglekey, angle).FromJust();
        Nan::Set(sample, distancekey, distance).FromJust();

        Nan::Set(samples, i, sample).FromJust();
      }

      const constexpr auto argc = 2u;
      v8::Local<v8::Value> argv[argc] = {Nan::Null(), samples};

      callback->Call(argc, argv);
    }

    Sweep& sweep;
    int32_t timeout;
    ::sweep_scan_s scan;
  };

  auto* callback = new Nan::Callback{function};
  Nan::AsyncQueueWorker(new AsyncScanWorker{*self, callback, timeout});
}

NAN_METHOD(Sweep::getMotorSpeed) {
  auto* const self = Nan::ObjectWrap::Unwrap<Sweep>(info.Holder());

  if (info.Length() != 0) {
    return Nan::ThrowTypeError("No arguments expected");
  }

  ::sweep_error_s error = NULL;
  const auto speed = ::sweep_device_get_motor_speed(self->device.get(), &error);

  if (error) {
    Nan::ThrowError(::sweep_error_message(error));
    ::sweep_error_destruct(error);
    return;
  }

  info.GetReturnValue().Set(Nan::New(speed));
}

NAN_METHOD(Sweep::setMotorSpeed) {
  auto* const self = Nan::ObjectWrap::Unwrap<Sweep>(info.Holder());

  if (info.Length() != 1 && !info[0]->IsNumber()) {
    return Nan::ThrowTypeError("Motor speed in Hz as number expected");
  }

  const auto speed = Nan::To<int32_t>(info[0]).FromJust();

  ::sweep_error_s error = NULL;
  ::sweep_device_set_motor_speed(self->device.get(), speed, &error);

  if (error) {
    Nan::ThrowError(::sweep_error_message(error));
    ::sweep_error_destruct(error);
    return;
  }
}

NAN_METHOD(Sweep::getSampleRate) {
  auto* const self = Nan::ObjectWrap::Unwrap<Sweep>(info.Holder());

  if (info.Length() != 0) {
    return Nan::ThrowTypeError("No arguments expected");
  }

  ::sweep_error_s error = NULL;
  const auto rate = ::sweep_device_get_sample_rate(self->device.get(), &error);

  if (error) {
    Nan::ThrowError(::sweep_error_message(error));
    ::sweep_error_destruct(error);
    return;
  }

  info.GetReturnValue().Set(Nan::New(rate));
}

NAN_METHOD(Sweep::reset) {
  auto* const self = Nan::ObjectWrap::Unwrap<Sweep>(info.Holder());

  if (info.Length() != 0) {
    return Nan::ThrowTypeError("No arguments expected");
  }

  ::sweep_error_s error = NULL;
  ::sweep_device_reset(self->device.get(), &error);

  if (error) {
    Nan::ThrowError(::sweep_error_message(error));
    ::sweep_error_destruct(error);
    return;
  }
}

Nan::Persistent<v8::Function>& Sweep::constructor() {
  static Nan::Persistent<v8::Function> init;
  return init;
}