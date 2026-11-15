package
{
   // Keys and pad buttons through xScal Input (Input.RegisterKey, IsKeyPressed, SuppressKey and the
   // gamepad calls), which read the keyboard and XInput directly. The calls go through the code object
   // the core module attaches to PluginSettings, so they fail until it is attached; register() is
   // repeated until it succeeds.
   public final class KeyInput
   {

      // Windows virtual-key codes.
      public static const KEY_ESCAPE:uint = 0x1B;

      public static const KEY_UP:uint = 0x26;

      public static const KEY_DOWN:uint = 0x28;

      public static const KEY_X:uint = 0x58;

      // XINPUT_GAMEPAD button bits.
      public static const PAD_DPAD_UP:uint = 0x0001;

      public static const PAD_DPAD_DOWN:uint = 0x0002;

      public static const PAD_X:uint = 0x4000;

      private static const KEYS:Array = [KEY_ESCAPE,KEY_UP,KEY_DOWN,KEY_X];

      // xScal registers a pad mask as a whole and AreGamepadButtonsPressed only answers for a
      // registered mask, so each button that isPadDown checks is registered on its own.
      private static const PAD_BUTTONS:Array = [PAD_DPAD_UP,PAD_DPAD_DOWN,PAD_X];

      // The object that holds __SFCodeObj.
      private var owner:Object;

      private var registered:Boolean = false;

      public function KeyInput(codeObjectOwner:Object)
      {
         super();
         this.owner = codeObjectOwner;
      }

      public function get isRegistered() : Boolean
      {
         return this.registered;
      }

      public function register() : Boolean
      {
         if(this.registered)
         {
            return true;
         }
         for each(var key:uint in KEYS)
         {
            if(this.call("Input.RegisterKey",key) !== true)
            {
               return false;
            }
         }
         for each(var buttons:uint in PAD_BUTTONS)
         {
            if(this.call("Input.RegisterGamepadButtons",buttons) !== true)
            {
               return false;
            }
         }
         this.registered = true;
         return true;
      }

      public function isKeyDown(key:uint) : Boolean
      {
         return this.call("Input.IsKeyPressed",key) === true;
      }

      public function isPadDown(buttons:uint) : Boolean
      {
         return this.call("Input.AreGamepadButtonsPressed",buttons) === true;
      }

      // Hides the key from the game; IsKeyPressed still reports it.
      public function suppressKey(key:uint) : void
      {
         this.call("Input.SuppressKey",key);
      }

      public function unsuppressKey(key:uint) : void
      {
         this.call("Input.UnsuppressKey",key);
      }

      private function call(name:String, value:uint) : *
      {
         try
         {
            return this.owner.__SFCodeObj.call(name,value);
         }
         catch(failure:Error)
         {
         }
         return null;
      }
   }
}
