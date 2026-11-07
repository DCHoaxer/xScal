package Shared.AS3
{
   import flash.display.MovieClip;
   
   // Compile-time declaration of the game's class from seventysixmenu.swf. Only the members
   // PluginSettings uses are declared. The stub is linked as external, so the game's class is used at run time.
   public class BSDisplayObject extends MovieClip
   {
      
      public function BSDisplayObject()
      {
         super();
      }
      
      public function get bIsDirty() : Boolean
      {
         return false;
      }
      
      public function SetIsDirty() : void
      {
      }
   }
}
