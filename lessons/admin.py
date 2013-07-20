from django.contrib import admin
from lessons.models import Lesson, Question, Example, AnswerFormat, LessonFollowsFromLesson

admin.site.register(Lesson)
admin.site.register(Question)
admin.site.register(Example)
admin.site.register(AnswerFormat)
admin.site.register(LessonFollowsFromLesson)
