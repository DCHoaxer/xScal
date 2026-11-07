// ActionJSON (c) 2010 Maximilian Herkender.
package com.brokenfunction.json
{
   import flash.system.ApplicationDomain;
   import flash.utils.ByteArray;
   
   public final class decodeJson
   {
      
      private static var byteInput:ByteArray;
      
      private static var position:uint = 0;
      
      private static var nativeJsonResolved:Boolean = false;
      
      private static var nativeJson:Object = null;
      
      public function decodeJson()
      {
         super();
         throw new Error("decodeJson is a static helper.");
      }
      
      public static function decode(input:*, allowNativeJson:Boolean = false) : *
      {
         if(input is String)
         {
            if(allowNativeJson)
            {
               resolveNativeJson();
               if(nativeJson != null)
               {
                  return nativeJson.parse(input);
               }
            }
            byteInput = new ByteArray();
            byteInput.writeUTFBytes(input as String);
         }
         else
         {
            if(!(input is ByteArray))
            {
               throw new Error("Unexpected input <" + input + ">");
            }
            byteInput = input as ByteArray;
         }
         position = 0;
         if(byteInput.length == 0)
         {
            throw new TypeError("Unexpected end of input at position 0");
         }
         var firstCharacter:uint = uint(byteInput[position]);
         position += 1;
         var result:* = parseValue(firstCharacter);
         skipWhitespace();
         if(position < byteInput.length)
         {
            throw new Error("Unexpected trailing input at position " + position);
         }
         return result;
      }
      
      private static function resolveNativeJson() : void
      {
         if(nativeJsonResolved)
         {
            return;
         }
         nativeJsonResolved = true;
         try
         {
            nativeJson = ApplicationDomain.currentDomain.getDefinition("JSON");
         }
         catch(error:Error)
         {
            nativeJson = null;
         }
      }
      
      private static function parseValue(firstCharacter:uint) : *
      {
         switch(firstCharacter)
         {
            case 34:
               return parseString();
            case 123:
               return parseObject();
            case 91:
               return parseArray();
            case 116:
               return parseTrue();
            case 102:
               return parseFalse();
            case 110:
               return parseNull();
            case 45:
            case 48:
            case 49:
            case 50:
            case 51:
            case 52:
            case 53:
            case 54:
            case 55:
            case 56:
            case 57:
               return parseNumber();
            case 9:
            case 10:
            case 13:
            case 32:
               skipWhitespace();
               if(position >= byteInput.length)
               {
                  throw new TypeError("Unexpected end of input at position " + position);
               }
               firstCharacter = uint(byteInput[position]);
               position += 1;
               return parseValue(firstCharacter);
               break;
            case 93:
               throw new Error("Unexpected end of array at " + (position - 1));
            case 125:
               throw new Error("Unexpected end of object at " + (position - 1));
            case 44:
               throw new Error("Unexpected comma at " + (position - 1));
            default:
               throw unexpectedCharacter(firstCharacter,position - 1);
         }
      }
      
      private static function parseObject() : Object
      {
         var result:Object = {};
         skipWhitespace();
         if(position < byteInput.length && byteInput[position] == 125)
         {
            position += 1;
            return result;
         }
         while(true)
         {
            if(position >= byteInput.length)
            {
               throw new Error("Unexpected end of object at " + position);
            }
            var keyStart:uint = uint(byteInput[position]);
            position += 1;
            if(keyStart != 34)
            {
               throw new Error("Expected string key at " + (position - 1));
            }
            var key:String = parseString();
            skipWhitespace();
            expectCharacter(58,"Expected :");
            skipWhitespace();
            if(position >= byteInput.length)
            {
               throw new Error("Unexpected end of object value at " + position);
            }
            var valueStart:uint = uint(byteInput[position]);
            position += 1;
            result[key] = parseValue(valueStart);
            skipWhitespace();
            if(position >= byteInput.length)
            {
               throw new Error("Unexpected end of object at " + position);
            }
            var delimiter:uint = uint(byteInput[position]);
            position += 1;
            if(delimiter == 125)
            {
               return result;
            }
            if(delimiter != 44)
            {
               throw new Error("Expected , or } at " + (position - 1));
            }
            skipWhitespace();
         }
         return result;
      }
      
      private static function parseArray() : Array
      {
         var result:Array = [];
         skipWhitespace();
         if(position < byteInput.length && byteInput[position] == 93)
         {
            position += 1;
            return result;
         }
         while(true)
         {
            if(position >= byteInput.length)
            {
               throw new Error("Unexpected end of array at " + position);
            }
            var valueStart:uint = uint(byteInput[position]);
            position += 1;
            result.push(parseValue(valueStart));
            skipWhitespace();
            if(position >= byteInput.length)
            {
               throw new Error("Unexpected end of array at " + position);
            }
            var delimiter:uint = uint(byteInput[position]);
            position += 1;
            if(delimiter == 93)
            {
               return result;
            }
            if(delimiter != 44)
            {
               throw new Error("Expected , or ] at " + (position - 1));
            }
            skipWhitespace();
         }
         return result;
      }
      
      private static function parseString() : String
      {
         var result:String = "";
         var segmentStart:uint = position;
         while(position < byteInput.length)
         {
            var character:uint = uint(byteInput[position]);
            position += 1;
            if(character == 34)
            {
               return result + readStringSegment(segmentStart,position - 1);
            }
            if(character == 92)
            {
               result += readStringSegment(segmentStart,position - 1);
               if(position >= byteInput.length)
               {
                  throw new Error("Unterminated escape sequence at " + position);
               }
               var escaped:uint = uint(byteInput[position]);
               position += 1;
               switch(escaped)
               {
                  case 34:
                     result += "\"";
                     break;
                  case 92:
                     result += "\\";
                     break;
                  case 47:
                     result += "/";
                     break;
                  case 98:
                     result += String.fromCharCode(8);
                     break;
                  case 102:
                     result += String.fromCharCode(12);
                     break;
                  case 110:
                     result += "\n";
                     break;
                  case 114:
                     result += "\r";
                     break;
                  case 116:
                     result += "\t";
                     break;
                  case 117:
                     result += parseUnicodeEscape();
                     break;
                  default:
                     throw new Error("Unknown escaped character encountered at position " + (position - 1));
               }
               segmentStart = position;
            }
            else if(character < 32)
            {
               throw new Error("Unescaped control character at position " + (position - 1));
            }
         }
         throw new Error("Unterminated string at position " + position);
      }
      
      private static function readStringSegment(start:uint, end:uint) : String
      {
         if(end <= start)
         {
            return "";
         }
         byteInput.position = start;
         return byteInput.readUTFBytes(end - start);
      }
      
      private static function parseUnicodeEscape() : String
      {
         if(position + 4 > byteInput.length)
         {
            throw new Error("Incomplete unicode escape at position " + position);
         }
         byteInput.position = position;
         var hexadecimal:String = byteInput.readUTFBytes(4);
         if(!/^[0-9A-Fa-f]{4}$/.test(hexadecimal))
         {
            throw new Error("Invalid unicode escape at position " + position);
         }
         position += 4;
         return String.fromCharCode(parseInt(hexadecimal,16));
      }
      
      private static function parseNumber() : Number
      {
         var start:uint = position - 1;
         while(position < byteInput.length && isNumberCharacter(byteInput[position]))
         {
            position += 1;
         }
         byteInput.position = start;
         var parsedNumber:Number = Number(byteInput.readUTFBytes(position - start));
         if(isNaN(parsedNumber))
         {
            throw new Error("Invalid number at position " + start);
         }
         return parsedNumber;
      }
      
      private static function isNumberCharacter(character:uint) : Boolean
      {
         return character == 43 || character == 45 || character == 46 || character >= 48 && character <= 57 || character == 69 || character == 101;
      }
      
      private static function parseTrue() : Boolean
      {
         expectSequence("rue","Expected true");
         return true;
      }
      
      private static function parseFalse() : Boolean
      {
         expectSequence("alse","Expected false");
         return false;
      }
      
      private static function parseNull() : *
      {
         expectSequence("ull","Expected null");
         return null;
      }
      
      private static function expectSequence(expected:String, errorMessage:String) : void
      {
         var index:int = 0;
         while(index < expected.length)
         {
            if(position >= byteInput.length)
            {
               throw new Error(errorMessage + " at position " + (position - 1));
            }
            var actual:uint = uint(byteInput[position]);
            position += 1;
            if(actual != expected.charCodeAt(index))
            {
               throw new Error(errorMessage + " at position " + (position - 1));
            }
            index++;
         }
      }
      
      private static function expectCharacter(expected:uint, errorMessage:String) : void
      {
         if(position >= byteInput.length)
         {
            throw new Error(errorMessage + " at position " + (position - 1));
         }
         var actual:uint = uint(byteInput[position]);
         position += 1;
         if(actual != expected)
         {
            throw new Error(errorMessage + " at position " + (position - 1));
         }
      }
      
      private static function skipWhitespace() : void
      {
         while(position < byteInput.length)
         {
            var character:uint = uint(byteInput[position]);
            if(character != 9 && character != 10 && character != 13 && character != 32)
            {
               return;
            }
            position += 1;
         }
      }
      
      private static function unexpectedCharacter(character:uint, atPosition:uint) : TypeError
      {
         return new TypeError("Unexpected character " + String.fromCharCode(character) + " (0x" + character.toString(16) + ") at position " + atPosition);
      }
   }
}

