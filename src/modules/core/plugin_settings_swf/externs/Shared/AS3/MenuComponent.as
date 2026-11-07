package Shared.AS3
{
   // Compile-time declaration of the game's class from seventysixmenu.swf.
   // The game declares the Active setter with an untyped argument; Boolean is used here so the
   // getter and setter types agree. The Active getter is only read, never overridden.
   public dynamic class MenuComponent extends BSUIComponent
   {
      
      protected var _active:Boolean = false;
      
      public function MenuComponent()
      {
         super();
      }
      
      public function get Active() : Boolean
      {
         return this._active;
      }
      
      public function set Active(value:Boolean) : void
      {
         this._active = value;
      }
      
      public function ProcessUserEvent(control:String, isDown:Boolean) : Boolean
      {
         return false;
      }
   }
}
