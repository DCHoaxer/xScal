package
{
   // Lines of the xScal credits from src/modules/core/credits.txt, which the core module builds into
   // the DLL and sets on PluginSettings.creditsText. Same layout as the game's credits.txt: every line
   // is shown as written. Lines that start with # are comments and are left out. A headline is the
   // first line of a block (after an empty line or at the start) when it has no lower-case letters,
   // for example "SPECIAL THANKS" or "<3 :)"; "CREATED BY DCHoaxer" and an all-capitals name further
   // down a block are not.
   public final class CreditsText
   {

      public function CreditsText()
      {
         super();
      }

      // Returns CreditsLine objects.
      public static function parse(text:String) : Array
      {
         var lines:Array = [];
         if(text == null)
         {
            return lines;
         }
         var startsBlock:Boolean = true;
         for each(var raw:String in text.split("\n"))
         {
            if(raw.length > 0 && raw.charAt(raw.length - 1) == "\r")
            {
               raw = raw.substr(0,raw.length - 1);
            }
            if(raw.charAt(0) == "#")
            {
               continue;
            }
            var empty:Boolean = isBlank(raw);
            lines.push(new CreditsLine(raw,startsBlock && !empty && !hasLowerCase(raw)));
            startsBlock = empty;
         }
         // The newline at the end of the file would add an empty last line.
         while(lines.length > 0 && lines[lines.length - 1].text == "")
         {
            lines.pop();
         }
         return lines;
      }

      private static function isBlank(line:String) : Boolean
      {
         return line.replace(/\s+/g,"").length == 0;
      }

      private static function hasLowerCase(line:String) : Boolean
      {
         return line != line.toUpperCase();
      }
   }
}
