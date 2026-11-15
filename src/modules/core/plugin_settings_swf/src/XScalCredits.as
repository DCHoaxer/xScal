package
{
   import Shared.AS3.MenuComponent;
   import flash.display.Bitmap;
   import flash.display.DisplayObject;
   import flash.display.Shape;
   import flash.display.Sprite;
   import flash.events.Event;
   import flash.events.MouseEvent;
   import flash.text.TextField;
   import flash.text.TextFormat;
   import flash.text.TextFormatAlign;
   import flash.utils.getTimer;

   // The xScal credits. Looks and moves like the game's SeventySixMenuCredits (CreditsMenu_mc of
   // seventysixmenu.swf): the logo, then white centered lines in $Credits_Font at size 24 that
   // scroll up over the menu background, fade in near the bottom and leave at the top. Up/Down and
   // the D-pad scroll faster either way, the mouse wheel moves the lines, Esc closes. A
   // MenuComponent, so the game's changeActiveComponent gives it stage focus and its Back (Tab, pad
   // B) reaches PluginSettings.
   public class XScalCredits extends MenuComponent
   {

      public static const FINISHED:String = "XScalCredits::FINISHED";

      // Values of SeventySixMenuCredits.
      private static const INPUT_SCROLL_RATE:Number = 10;

      private static const START_HEIGHT:Number = -100;

      private static const START_Y:Number = 1000;

      private static const FADE_START:Number = 990;

      private static const FADE_END:Number = 900;

      private static const END_Y:Number = -25;

      private static const TOP_START_Y:Number = 0;

      private static const FONT_SIZE:int = 24;

      private static const TEXT_COLOR:uint = 0xFFFFFF;

      // Headlines (see CreditsText) use the yellow of the game's menu titles, as in the settings window.
      private static const HEADLINE_COLOR:uint = 0xF5CB5B;

      private static const WHEEL_STEP:Number = 20;

      // Stage size of seventysixmenu.swf and seventysixmenucredits.swf.
      private static const STAGE_WIDTH:Number = 1920;

      private static const STAGE_HEIGHT:Number = 1080;

      // A TextField draws its text 2 pixels in from each edge.
      private static const TEXT_GUTTER:Number = 4;

      // Length of the Nuka theme the core module plays during the credits (2:28). The scroll speed is set
      // so the last line leaves the screen when the track ends, kept between MIN_SCROLL_SPEED and
      // MAX_SCROLL_SPEED pixels per second for a short or a long plugin list.
      private static const TRACK_SECONDS:Number = 148;

      private static const MIN_SCROLL_SPEED:Number = 20;

      private static const MAX_SCROLL_SPEED:Number = 60;

      // Longest frame time used for one step, so a hitch does not make the lines jump.
      private static const MAX_STEP_MS:int = 100;

      private static const LOGO_GAP:Number = 24;

      [Embed(source="../assets/xscal_logo.png")]
      private static const LogoImage:Class;

      // One picture per Fallout game, in release order, shown beside the lines on the left, then the
      // right, then the left again. The PNGs are 180x240 crops with ragged feathered edges, in
      // grayscale with 32 gray and alpha combinations, which build.sh then stores as 8-bit indexes
      // (index_bitmaps.py).
      // They are embedded lossless like the logo: as JPEG with alpha (compression="true", DefineBitsJPEG3) the game's
      // Scaleform drew nothing.
      [Embed(source="../assets/fallout1.png")]
      private static const Fallout1Image:Class;

      [Embed(source="../assets/fallout2.png")]
      private static const Fallout2Image:Class;

      [Embed(source="../assets/fallout3.png")]
      private static const Fallout3Image:Class;

      [Embed(source="../assets/fallout_nv.png")]
      private static const FalloutNewVegasImage:Class;

      [Embed(source="../assets/fallout4.png")]
      private static const Fallout4Image:Class;

      [Embed(source="../assets/fallout76.png")]
      private static const Fallout76Image:Class;

      private static const MEMENTO_WIDTH:Number = 180;

      // Distance of a picture from the left or right stage edge. Kept away from the edge because the
      // sides of the 1920-wide stage are cut on screens narrower than 16:9.
      private static const MEMENTO_MARGIN:Number = 150;

      // Free space between a picture and the nearest text.
      private static const MEMENTO_TEXT_GAP:Number = 40;

      // Smallest vertical distance between the tops of two pictures that follow each other.
      private static const MEMENTO_STAGGER:Number = 240;

      // Step used when looking for a free place for a picture.
      private static const MEMENTO_SEARCH_STEP:Number = 12;

      // src/modules/core/credits.txt, set through PluginSettings.creditsText.
      public var text:String = "";

      public var onEscape:Function;

      public var input:KeyInput;

      private var container:Sprite = new Sprite();

      private var logo:Bitmap;

      private var lines:Vector.<DisplayObject> = new Vector.<DisplayObject>();

      // Picture of each Fallout game, built once.
      private var mementos:Vector.<Bitmap> = new Vector.<Bitmap>();

      // The last text line. The scroll speed and the end of the credits follow it, not the pictures.
      private var lastLine:DisplayObject;

      // Pixels per second. The lines move by elapsed time, so a slow frame does not slow the scroll down.
      private var scrollSpeed:Number = MIN_SCROLL_SPEED;

      private var lastFrameAt:int = 0;

      private var running:Boolean = false;

      // Starts true at each open, so an Esc held from before the credits does not close them.
      private var escapeHeld:Boolean = true;

      public function XScalCredits()
      {
         super();
         this.visible = false;
         // Transparent, like Background_mc of the game's credits: it only catches the mouse wheel.
         var wheelArea:Shape = new Shape();
         wheelArea.graphics.beginFill(0,0);
         wheelArea.graphics.drawRect(0,0,STAGE_WIDTH,STAGE_HEIGHT);
         wheelArea.graphics.endFill();
         this.addChild(wheelArea);
         this.addChild(this.container);
         this.logo = new LogoImage() as Bitmap;
         this.logo.smoothing = true;
         this.mementos.push(createMemento(Fallout1Image));
         this.mementos.push(createMemento(Fallout2Image));
         this.mementos.push(createMemento(Fallout3Image));
         this.mementos.push(createMemento(FalloutNewVegasImage));
         this.mementos.push(createMemento(Fallout4Image));
         this.mementos.push(createMemento(Fallout76Image));
         this.addEventListener(MouseEvent.MOUSE_WHEEL,this.onMouseWheel);
      }

      public function get isEscapeHeld() : Boolean
      {
         return this.escapeHeld;
      }

      // The credits have no inner level to leave on Back.
      public function leaveOptions() : Boolean
      {
         return false;
      }

      public function prepareToOpen() : void
      {
         this.clearLines();
         this.escapeHeld = true;
         this.container.y = START_HEIGHT;
         this.logo.x = (STAGE_WIDTH - this.logo.width) / 2;
         // The logo fades in by its top edge and starts at FADE_START, so it shows from the first frames.
         // By its bottom edge, like a text line, the 344-pixel logo stayed invisible for seconds.
         this.logo.y = FADE_START - START_HEIGHT;
         this.logo.alpha = 0;
         this.logo.visible = true;
         this.container.addChild(this.logo);
         this.lines.push(this.logo);
         var texts:Array = CreditsText.parse(this.text);
         var textTop:Number = this.logo.y + this.logo.height + LOGO_GAP;
         var y:Number = textTop;
         var fields:Vector.<TextField> = new Vector.<TextField>();
         for each(var line:CreditsLine in texts)
         {
            var field:TextField = createLine(line.text,line.isHeadline ? HEADLINE_COLOR : TEXT_COLOR);
            field.y = y;
            y += field.height;
            this.container.addChild(field);
            this.lines.push(field);
            fields.push(field);
         }
         this.lastLine = this.lines[this.lines.length - 1];
         this.placeMementos(fields,textTop,y);
         // The scroll ends when the bottom of the last line reaches END_Y.
         var distance:Number = y + START_HEIGHT - END_Y;
         this.scrollSpeed = Math.max(MIN_SCROLL_SPEED,Math.min(MAX_SCROLL_SPEED,distance / TRACK_SECONDS));
         this.updateLines();
         // The game must not see Esc as its Cancel while the credits handle it.
         this.input.suppressKey(KeyInput.KEY_ESCAPE);
         this.running = true;
         this.lastFrameAt = getTimer();
         this.addEventListener(Event.ENTER_FRAME,this.onEnterFrame);
      }

      // Called when the credits close, after the Esc that closed them is released.
      public function releaseEscapeListeners() : void
      {
         this.input.unsuppressKey(KeyInput.KEY_ESCAPE);
         this.running = false;
         this.removeEventListener(Event.ENTER_FRAME,this.onEnterFrame);
         this.clearLines();
      }

      // The field spans the stage and centers its text, so every line is centered on the screen. Height
      // follows the text; an empty line keeps the height of a text line, like the gaps in the game's
      // credits.
      private static function createLine(text:String, color:uint) : TextField
      {
         var format:TextFormat = new TextFormat("$Credits_Font",FONT_SIZE,color);
         format.align = TextFormatAlign.CENTER;
         var field:TextField = new TextField();
         field.selectable = false;
         field.mouseEnabled = false;
         field.embedFonts = false;
         field.multiline = false;
         field.wordWrap = false;
         field.x = 0;
         field.width = STAGE_WIDTH;
         field.defaultTextFormat = format;
         field.text = text.length > 0 ? text : " ";
         field.setTextFormat(format);
         field.textColor = color;
         field.height = field.textHeight + TEXT_GUTTER;
         return field;
      }

      private static function createMemento(imageClass:Class) : Bitmap
      {
         var image:Bitmap = new imageClass() as Bitmap;
         image.smoothing = true;
         return image;
      }

      // Spreads the pictures evenly over the text, from textTop to textBottom, alternating left and
      // right. Each picture moves down (or, failing that, up) from its even place until no text line
      // beside it is wide enough to touch it. A picture with no free place is left out. Every picture
      // ends above textBottom, so the last text line still leaves the screen last.
      private function placeMementos(fields:Vector.<TextField>, textTop:Number, textBottom:Number) : void
      {
         var slot:Number = (textBottom - textTop) / this.mementos.length;
         var minTop:Number = textTop;
         for(var i:int = 0; i < this.mementos.length; i++)
         {
            var memento:Bitmap = this.mementos[i];
            var mementoHeight:Number = memento.height;
            var maxTop:Number = textBottom - mementoHeight;
            if(maxTop < minTop)
            {
               break;
            }
            var wanted:Number = Math.min(maxTop,Math.max(minTop,textTop + slot * (i + 0.5) - mementoHeight / 2));
            var top:Number = findFreeTop(fields,wanted,minTop,maxTop,mementoHeight);
            if(isNaN(top))
            {
               continue;
            }
            memento.x = i % 2 == 0 ? MEMENTO_MARGIN : STAGE_WIDTH - MEMENTO_MARGIN - MEMENTO_WIDTH;
            memento.y = top;
            memento.alpha = 0;
            memento.visible = true;
            this.container.addChild(memento);
            this.lines.push(memento);
            minTop = top + MEMENTO_STAGGER;
         }
      }

      // Returns the top nearest to wanted, first searching down to maxTop and then up to minTop, where
      // a picture of the given height has no wide text line beside it, or NaN when there is none.
      private static function findFreeTop(fields:Vector.<TextField>, wanted:Number, minTop:Number, maxTop:Number, height:Number) : Number
      {
         var top:Number;
         for(top = wanted; top <= maxTop; top += MEMENTO_SEARCH_STEP)
         {
            if(isBesideNarrowLines(fields,top,height))
            {
               return top;
            }
         }
         for(top = wanted - MEMENTO_SEARCH_STEP; top >= minTop; top -= MEMENTO_SEARCH_STEP)
         {
            if(isBesideNarrowLines(fields,top,height))
            {
               return top;
            }
         }
         return NaN;
      }

      // The lines are centered, so one width limit holds for both sides.
      private static function isBesideNarrowLines(fields:Vector.<TextField>, top:Number, height:Number) : Boolean
      {
         var maxHalfWidth:Number = STAGE_WIDTH / 2 - MEMENTO_MARGIN - MEMENTO_WIDTH - MEMENTO_TEXT_GAP;
         var bottom:Number = top + height;
         for each(var field:TextField in fields)
         {
            if(field.y >= bottom || field.y + field.height <= top)
            {
               continue;
            }
            if(field.textWidth / 2 > maxHalfWidth)
            {
               return false;
            }
         }
         return true;
      }

      private function clearLines() : void
      {
         this.container.removeChildren();
         this.lines.length = 0;
      }

      private function onMouseWheel(event:MouseEvent) : void
      {
         if(this.running)
         {
            this.moveLines(WHEEL_STEP * event.delta);
         }
      }

      private function onEnterFrame(event:Event) : void
      {
         var escapeDown:Boolean = this.input.isKeyDown(KeyInput.KEY_ESCAPE);
         var escapePressed:Boolean = escapeDown && !this.escapeHeld;
         this.escapeHeld = escapeDown;
         if(escapePressed)
         {
            this.onEscape();
         }
         if(!this.running)
         {
            return;
         }
         var last:DisplayObject = this.lastLine;
         if(this.container.y + last.y + last.height <= END_Y)
         {
            this.running = false;
            dispatchEvent(new Event(FINISHED));
            return;
         }
         var now:int = getTimer();
         var step:Number = this.scrollSpeed * Math.min(now - this.lastFrameAt,MAX_STEP_MS) / 1000;
         this.lastFrameAt = now;
         if(this.input.isKeyDown(KeyInput.KEY_UP) || this.input.isPadDown(KeyInput.PAD_DPAD_UP))
         {
            this.moveLines(step * INPUT_SCROLL_RATE);
         }
         else if(this.input.isKeyDown(KeyInput.KEY_DOWN) || this.input.isPadDown(KeyInput.PAD_DPAD_DOWN))
         {
            this.moveLines(-step * INPUT_SCROLL_RATE);
         }
         else
         {
            this.moveLines(-step);
         }
      }

      // Same limit as the game: the lines cannot move further down than their start.
      private function moveLines(distance:Number) : void
      {
         this.container.y = Math.min(this.container.y + distance,TOP_START_Y);
         this.updateLines();
      }

      // Hides lines outside the screen and fades the ones near the bottom, by the bottom edge of each
      // line as in SeventySixMenuCredits.moveCredits (the logo and the Fallout pictures by their top
      // edge). visible and alpha are written only when they change: writing them on every line each
      // frame made the scroll stutter.
      private function updateLines() : void
      {
         for each(var line:DisplayObject in this.lines)
         {
            var top:Number = this.container.y + line.y;
            var bottom:Number = top + line.height;
            var shown:Boolean = bottom > END_Y && top < START_Y;
            if(line.visible != shown)
            {
               line.visible = shown;
            }
            if(!shown)
            {
               continue;
            }
            var edge:Number = line is TextField ? bottom : top;
            var alpha:Number;
            if(edge <= FADE_END)
            {
               alpha = 1;
            }
            else if(edge >= FADE_START)
            {
               alpha = 0;
            }
            else
            {
               alpha = 1 - (edge - FADE_END) / (FADE_START - FADE_END);
            }
            if(line.alpha != alpha)
            {
               line.alpha = alpha;
            }
         }
      }
   }
}
