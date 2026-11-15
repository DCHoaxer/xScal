package
{
   // One line of the xScal credits, see CreditsText.
   public final class CreditsLine
   {

      public var text:String;

      public var isHeadline:Boolean;

      public function CreditsLine(lineText:String, headline:Boolean)
      {
         super();
         this.text = lineText;
         this.isHeadline = headline;
      }
   }
}
